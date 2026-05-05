// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// scsi.c
// Implements SCSI controller emulation for the NCR 5380

#define _CRT_SECURE_NO_WARNINGS 1

#include "scsi.h"
#include "drive_catalog.h"
#include "image.h"
#include "log.h"
#include "object.h"
#include "platform.h"
#include "scsi_internal.h"
#include "shell.h"
#include "system.h"
#include "system_config.h"
#include "value.h"

extern config_t *global_emulator;

// Forward declarations — class descriptors are at the bottom of the file but
// scsi_init / scsi_delete reference them.
extern const class_desc_t scsi_class;
extern const class_desc_t scsi_bus_class;
extern const class_desc_t scsi_devices_collection_class;
extern const class_desc_t scsi_device_class;

#include <assert.h>
#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Constants and Macros
// ============================================================================

LOG_USE_CATEGORY_NAME("scsi");
// SCSI loopback trace — enable selectively for debugging
#define SCSI_TRACE(...)                                                                                                \
    do {                                                                                                               \
        (void)0;                                                                                                       \
    } while (0)

// ============================================================================
// Static Helpers
// ============================================================================

// Determine the length of a SCSI command based on its opcode
static int cmd_size(uint8_t opcode) {
    return opcode < 0x20 ? 6 : 10;
}

// Compute the CDR value from the current loopback state.
// On the Apple Loopback Test Card, control signal pins are wired to specific
// data bus pins.  CDR also reflects ODR when the data bus driver is active.
static uint8_t compute_loopback_cdr(scsi_t *scsi) {
    uint8_t val = 0;
    // ODR driven onto data bus when DB asserted or in target mode
    if ((scsi->reg.icr & ICR_DB) || (scsi->reg.mr & MR_TARGET))
        val |= scsi->reg.odr;
    // Loopback card wiring: ICR control signal → data bus pin
    if (scsi->reg.icr & ICR_ATN)
        val |= 0x40; // ATN → DB6
    if (scsi->reg.icr & ICR_ACK)
        val |= 0x20; // ACK → DB5
    if (scsi->reg.icr & ICR_BSY)
        val |= 0x04; // BSY → DB2
    if (scsi->reg.icr & ICR_SEL)
        val |= 0x10; // SEL → DB4
    // Target mode: TCR signals → data bus pins
    if (scsi->reg.mr & MR_TARGET) {
        if (scsi->reg.tcr & 0x01)
            val |= 0x80; // I/O → DB7
        if (scsi->reg.tcr & 0x02)
            val |= 0x02; // C/D → DB1
        if (scsi->reg.tcr & 0x04)
            val |= 0x08; // MSG → DB3
        if (scsi->reg.tcr & 0x08)
            val |= 0x01; // REQ → DB0
    }
    return val;
}

// Compute BSR phase-match bit: true when bus phase matches TCR
static bool scsi_phase_match(scsi_t *scsi) {
    // TCR bits 2:0 = MSG, C/D, I/O  (written by initiator)
    // CSR bits 4:2 = MSG, C/D, I/O  (actual bus signals)
    // In loopback mode, CSR is computed dynamically from ICR/TCR — use
    // the live bus signals rather than the stored csr register.
    uint8_t csr = scsi->reg.csr;
    if (scsi->loopback && (scsi->reg.mr & MR_TARGET)) {
        if (scsi->reg.tcr & 0x01)
            csr |= CSR_IO;
        if (scsi->reg.tcr & 0x02)
            csr |= CSR_CD;
        if (scsi->reg.tcr & 0x04)
            csr |= CSR_MSG;
    }
    return ((csr >> 2) & 7) == (scsi->reg.tcr & 7);
}

// Drive VIA2 CB2 (SCSI /IRQ) based on current interrupt conditions.
// The NCR 5380 asserts /IRQ on phase mismatch during DMA, end of DMA,
// loss of BSY during DMA, or bus reset detection.
static void scsi_update_irq(scsi_t *scsi) {
    // Phase mismatch during DMA mode is the primary IRQ source
    bool irq = false;
    if (scsi->reg.mr & MR_DMA) {
        if (!scsi_phase_match(scsi))
            irq = true; // phase mismatch during DMA
        if (scsi->end_of_dma)
            irq = true; // end of DMA
    }

    if (irq == scsi->irq_active)
        return;
    scsi->irq_active = irq;

    // Drive VIA2 CB2: active-low (0 = asserted)
    if (scsi->via)
        via_input_c(scsi->via, 1, 1, !irq);
}

// Drive VIA2 CA2 (SCSI /DRQ) based on DMA data readiness.
// Asserted when DMA mode is enabled and data is available for transfer.
static void scsi_update_drq(scsi_t *scsi) {
    bool drq = (scsi->reg.bsr & BSR_DR) != 0;

    if (drq == scsi->drq_active)
        return;
    scsi->drq_active = drq;

    // Drive VIA2 CA2: active-low (0 = asserted)
    if (scsi->via)
        via_input_c(scsi->via, 0, 1, !drq);
}

// Pop the next byte from the SCSI buffer
static uint8_t next_byte(scsi_t *scsi) {
    assert(scsi->buf.size > 0);

    uint8_t byte = scsi->buf.data[0];
    memmove(scsi->buf.data, scsi->buf.data + 1, --scsi->buf.size);

    return byte;
}

// Transition SCSI bus to the free/idle state
static void phase_free(scsi_t *scsi) {
    scsi->bus.phase = scsi_bus_free;
    scsi->reg.csr = 0;
    scsi->end_of_dma = false;
    scsi_update_drq(scsi);
    scsi_update_irq(scsi);
}

// Transition SCSI bus to arbitration phase
static void phase_arbitration(scsi_t *scsi) {
    assert(scsi->bus.phase == scsi_bus_free);

    scsi->bus.phase = scsi_arbitration;
}

// Transition SCSI bus to selection phase (from arbitration or bus-free for non-arbitrated selection)
static void phase_selection(scsi_t *scsi) {
    assert(scsi->bus.phase == scsi_arbitration || scsi->bus.phase == scsi_bus_free);

    scsi->bus.phase = scsi_selection;
}

// Transition SCSI bus to command phase
static void phase_command(scsi_t *scsi) {
    assert(scsi->bus.phase == scsi_selection);

    // by not asserting MSG, we indicate that we don't support any messages (other than command complete)
    // i.e. go directly to the command phase
    scsi->reg.csr = CSR_CD + CSR_REQ + CSR_BSY;

    // reset the buffer - will hold the command
    scsi->buf.max = MAX_CMD_SIZE;
    scsi->buf.size = 0;
    scsi->bus.phase = scsi_command;
    scsi_update_irq(scsi);
}

// Transition SCSI bus to data-in phase (target to initiator)
void phase_data_in(scsi_t *scsi, int bytes) {
    assert(scsi->bus.phase == scsi_command);

    scsi->bus.phase = scsi_data_in;
    scsi->reg.csr = CSR_IO + CSR_REQ + CSR_BSY;
    scsi->buf.size = scsi->buf.max = bytes;
    // Skip scsi_update_irq: prevents spurious phase-mismatch IRQ when
    // run_cmd fires during pseudo-DMA ODR write with MR_DMA still set
    // for command phase.
}

// Transition SCSI bus to data-out phase (initiator to target)
void phase_data_out(scsi_t *scsi, int bytes) {
    assert(scsi->bus.phase == scsi_command);
    assert(bytes <= BUF_LIMIT);

    scsi->bus.phase = scsi_data_out;
    scsi->reg.csr = CSR_REQ + CSR_BSY;
    scsi->buf.max = bytes;
    scsi->buf.size = 0;
    // Arm the primer-slot gate.  See scsi_internal.h `primer_held` etc.
    scsi->primer_held = false;
    // Same rationale as phase_data_in.
}

// Transition SCSI bus to message-out phase (initiator to target).
// SCSI targets enter this phase when the initiator asserts ATN,
// allowing the initiator to send messages such as IDENTIFY or ABORT.
static void phase_message_out(scsi_t *scsi) {
    scsi->bus.saved_phase = scsi->bus.phase;
    scsi->bus.phase = scsi_message_out;
    // MSG + C/D + REQ + BSY, no I/O (direction is initiator → target)
    scsi->reg.csr = CSR_MSG + CSR_CD + CSR_REQ + CSR_BSY;
    scsi_update_irq(scsi);
}

// Transition SCSI bus to status phase
void phase_status(scsi_t *scsi, uint8_t status) {
    assert(scsi->bus.phase == scsi_command || scsi->bus.phase == scsi_data_in || scsi->bus.phase == scsi_data_out ||
           scsi->bus.phase == scsi_message_out);

    bool was_data_in = (scsi->bus.phase == scsi_data_in);

    scsi->bus.phase = scsi_status;
    scsi->reg.csr = CSR_IO + CSR_CD + CSR_REQ + CSR_BSY;
    scsi->reg.cdr = status;
    // Signal end-of-DMA to the IRQ logic if DMA was active
    if (scsi->reg.mr & MR_DMA)
        scsi->end_of_dma = true;

    // When transitioning from data-in with DMA active, skip scsi_update_irq.
    // On real NCR 5380 hardware the target changes bus phase only AFTER
    // the final ACK handshake completes — the phase-mismatch IRQ fires
    // asynchronously, not during the register read that returns the last
    // data byte.  A/UX's pseudo-DMA loop (scsiin) runs at IPL 0 and
    // would service the IRQ immediately, causing a nested scsidintr that
    // stamps SST_MORE.  Mac OS runs its pseudo-DMA at IPL >= 2, so the
    // IRQ is masked and harmlessly deferred; skipping it has no effect.
    // The host clears MR_DMA next (write_mr), which fires scsi_update_irq
    // with DMA off — the correct post-transfer notification.
    if (was_data_in && (scsi->reg.mr & MR_DMA)) {
        return;
    }

    scsi_update_irq(scsi);
}

// Transition SCSI bus to message-in phase
void phase_message_in(scsi_t *scsi, uint8_t message) {
    assert(scsi->bus.phase == scsi_status);

    scsi->bus.phase = scsi_message_in;
    scsi->reg.csr = CSR_IO + CSR_CD + CSR_MSG + CSR_REQ + CSR_BSY;
    scsi->reg.cdr = message;
    scsi_update_irq(scsi);
}

// Set sense data for a device
void scsi_set_sense(scsi_t *scsi, int target, uint8_t key, uint8_t asc, uint8_t ascq) {
    scsi->devices[target & 7].sense.key = key;
    scsi->devices[target & 7].sense.asc = asc;
    scsi->devices[target & 7].sense.ascq = ascq;
}

// Return CHECK CONDITION, setting sense data on the current target
void scsi_check_condition(scsi_t *scsi, uint8_t sense_key, uint8_t asc, uint8_t ascq) {
    scsi_set_sense(scsi, scsi->bus.target, sense_key, asc, ascq);
    phase_status(scsi, STATUS_CHECK_CONDITION);
}

// Execute a SCSI command after receiving it from the initiator
static void run_cmd(scsi_t *scsi) {
    scsi->cmd.opcode = scsi->buf.data[0];
    int target = scsi->bus.target & 7;

    // Check for pending UNIT ATTENTION on first non-exempt command
    // INQUIRY and REQUEST SENSE are exempt per SCSI spec
    if (scsi->devices[target].unit_attention && scsi->cmd.opcode != CMD_INQUIRY &&
        scsi->cmd.opcode != CMD_REQUEST_SENSE) {
        scsi->devices[target].unit_attention = false;
        scsi_check_condition(scsi, SENSE_UNIT_ATTENTION, ASC_NOT_READY_TO_READY, 0x00);
        return;
    }

    switch (scsi->cmd.opcode) {

    case CMD_TEST_UNIT_READY:
        LOG(1, "command: TEST UNIT READY");
        // Check if medium is present for CD-ROM
        if (scsi->devices[target].type == scsi_dev_cdrom && !scsi->devices[target].medium_present) {
            scsi_check_condition(scsi, SENSE_NOT_READY, ASC_MEDIUM_NOT_PRESENT, 0x00);
        } else {
            phase_status(scsi, STATUS_GOOD);
        }
        break;

    case CMD_REZERO_UNIT:
        LOG(1, "command: REZERO UNIT");
        // Seek to block 0 — no-op in emulation
        phase_status(scsi, STATUS_GOOD);
        break;

    case CMD_REQUEST_SENSE:
        scsi_cdrom_request_sense(scsi);
        break;

    case CMD_FORMAT_UNIT:
        LOG(1, "command: FORMAT UNIT");
        // Reject FORMAT on read-only devices
        if (scsi->devices[target].read_only) {
            scsi_check_condition(scsi, SENSE_DATA_PROTECT, ASC_WRITE_PROTECTED, 0x00);
        } else {
            phase_status(scsi, STATUS_GOOD);
        }
        break;

    case CMD_READ:
    case CMD_WRITE: {

        scsi->cmd.lun = scsi->buf.data[1] >> 5;
        scsi->cmd.lba = scsi->buf.data[1] << 16 & 0x1FFFFF | scsi->buf.data[2] << 8 | scsi->buf.data[3];
        scsi->cmd.tl = scsi->cmd.tl = (scsi->buf.data[4] - 1 & 0xFF) + 1;

        assert((scsi->buf.data[5] & 0x3) == 0); // FLAG = LINK = 0
        assert(scsi->cmd.lun == 0);

        uint16_t blk_sz = scsi->devices[target].block_size;

        LOG(1, "SCSI %s target=%d lba=%u tl=%u blk_sz=%u raw_size=%zu",
            scsi->cmd.opcode == CMD_WRITE ? "WRITE" : "READ", target, scsi->cmd.lba, scsi->cmd.tl, blk_sz,
            scsi->devices[target].image ? scsi->devices[target].image->raw_size : 0);

        // Reject writes on read-only devices (CD-ROM, etc.)
        if (scsi->cmd.opcode == CMD_WRITE && scsi->devices[target].read_only) {
            scsi_check_condition(scsi, SENSE_DATA_PROTECT, ASC_WRITE_PROTECTED, 0x00);
            break;
        }

        assert(scsi->cmd.tl * blk_sz <= BUF_LIMIT);

        if (scsi->cmd.opcode == CMD_WRITE) {
            phase_data_out(scsi, blk_sz * scsi->cmd.tl);
        } else {
            phase_data_in(scsi, scsi->cmd.tl * blk_sz);
            size_t byte_off = (size_t)scsi->cmd.lba * blk_sz;
            size_t byte_cnt = (size_t)scsi->cmd.tl * blk_sz;
            // bounds check: reject out-of-range reads
            if (byte_off + byte_cnt > scsi->devices[target].image->raw_size) {
                LOG(1, "SCSI READ out of range: target=%d lba=%u byte_off=%zu byte_cnt=%zu raw_size=%zu", target,
                    scsi->cmd.lba, byte_off, byte_cnt, scsi->devices[target].image->raw_size);
                scsi_check_condition(scsi, SENSE_ILLEGAL_REQUEST, ASC_LBA_OUT_OF_RANGE, 0x00);
                break;
            }
            size_t n = disk_read_data(scsi->devices[target].image, byte_off, scsi->buf.data, byte_cnt);
            assert(n == byte_cnt);
        }
        break;
    }

    case CMD_READ_10:
    case CMD_WRITE_10: {
        // 10-byte CDB: LBA in bytes 2-5, transfer length in bytes 7-8
        scsi->cmd.lba =
            (scsi->buf.data[2] << 24) | (scsi->buf.data[3] << 16) | (scsi->buf.data[4] << 8) | scsi->buf.data[5];
        scsi->cmd.tl = (scsi->buf.data[7] << 8) | scsi->buf.data[8];

        uint16_t blk_sz = scsi->devices[target].block_size;

        // Reject writes on read-only devices
        if (scsi->cmd.opcode == CMD_WRITE_10 && scsi->devices[target].read_only) {
            scsi_check_condition(scsi, SENSE_DATA_PROTECT, ASC_WRITE_PROTECTED, 0x00);
            break;
        }

        if (scsi->cmd.tl == 0) {
            // Transfer length 0: no data transfer, just return good status
            phase_status(scsi, STATUS_GOOD);
            break;
        }

        assert(scsi->cmd.tl * blk_sz <= BUF_LIMIT);

        if (scsi->cmd.opcode == CMD_WRITE_10) {
            phase_data_out(scsi, blk_sz * scsi->cmd.tl);
        } else {
            phase_data_in(scsi, scsi->cmd.tl * blk_sz);
            size_t byte_off = (size_t)scsi->cmd.lba * blk_sz;
            size_t byte_cnt = (size_t)scsi->cmd.tl * blk_sz;
            // bounds check: reject out-of-range reads
            if (byte_off + byte_cnt > scsi->devices[target].image->raw_size) {
                LOG(1, "SCSI READ_10 out of range: target=%d lba=%u byte_off=%zu byte_cnt=%zu raw_size=%zu", target,
                    scsi->cmd.lba, byte_off, byte_cnt, scsi->devices[target].image->raw_size);
                scsi_check_condition(scsi, SENSE_ILLEGAL_REQUEST, ASC_LBA_OUT_OF_RANGE, 0x00);
                break;
            }
            size_t n = disk_read_data(scsi->devices[target].image, byte_off, scsi->buf.data, byte_cnt);
            assert(n == byte_cnt);
        }
        break;
    }

    case CMD_SEEK_6:
    case CMD_SEEK_10:
        // Seek to LBA — no-op (no seek latency to emulate)
        LOG(1, "command: SEEK");
        phase_status(scsi, STATUS_GOOD);
        break;

    case CMD_INQUIRY:

        assert(scsi->buf.data != NULL);
        // INQUIRY must respond regardless of media presence — per SCSI-2 the
        // response is built from the static vendor/product/revision fields and
        // device type, not from the backing image.  Asserting on image != NULL
        // would crash any future probe of a present-but-empty target.

        // [6]: byte 4 is the "allocation length"
        scsi->cmd.tl = scsi->buf.data[4];
        if (scsi->cmd.tl == 0)
            scsi->cmd.tl = 36; // default allocation length
        phase_data_in(scsi, scsi->cmd.tl);

        memset(scsi->buf.data, 0, scsi->cmd.tl);

        if (scsi->devices[target].type == scsi_dev_cdrom) {
            // CD-ROM INQUIRY: device type 0x05, removable media
            scsi->buf.data[0] = 0x05; // CD-ROM device type
            scsi->buf.data[1] = 0x80; // removable media bit (RMB)
            scsi->buf.data[2] = 0x01; // SCSI-1 (ANSI version)
            scsi->buf.data[3] = 0x01; // response data format: CCS
            scsi->buf.data[4] = 0x31; // additional length: 49 bytes
            // Bytes 5-7: zero
            if (scsi->cmd.tl >= 36) {
                memcpy(scsi->buf.data + 8, scsi->devices[target].vendor_id, 8);
                memcpy(scsi->buf.data + 16, scsi->devices[target].product_id, 16);
                memcpy(scsi->buf.data + 32, scsi->devices[target].revision, 4);
            }
        } else {
            // Hard disk INQUIRY: device type 0x00
            scsi->buf.data[0] = 0x00; // direct-access device
            scsi->buf.data[1] = 0x00; // non-removable media
            scsi->buf.data[2] = 0x01; // SCSI-1 (ANSI version)
            scsi->buf.data[3] = 0x01; // response data format: CCS
            scsi->buf.data[4] = 0x31; // additional length: 49 bytes
            if (scsi->cmd.tl >= 36) {
                memcpy(scsi->buf.data + 8, scsi->devices[target].vendor_id, 8);
                memcpy(scsi->buf.data + 16, scsi->devices[target].product_id, 16);
                memcpy(scsi->buf.data + 32, scsi->devices[target].revision, 4);
            }
        }

        break;

    case CMD_MODE_SELECT: {
        // MODE SELECT(6) byte 4 is the parameter list length.  Zero means
        // no data phase — A/UX's HD driver issues this as a no-op probe.
        int param_len = scsi->buf.data[4];
        if (param_len == 0)
            phase_status(scsi, STATUS_GOOD);
        else
            phase_data_out(scsi, param_len);
        break;
    }

    case CMD_MODE_SENSE: {
        // MODE SENSE(6): dispatch based on device type
        if (scsi->devices[target].type == scsi_dev_cdrom) {
            scsi_cdrom_mode_sense(scsi);
        } else {
            // HD MODE SENSE(6): CDB byte 2 bits 5:0 = page code
            uint8_t page_code = scsi->buf.data[2] & 0x3F;
            uint32_t blocks = (uint32_t)(disk_size(scsi->devices[target].image) / 512);

            if (page_code == 0x30) {
                // Vendor-specific page 0x30: Apple drive identification
                // Apple HD SC Setup checks this page for "APPLE COMPUTER, INC."
                static const char apple_id[] = "APPLE COMPUTER, INC.";
                int page_len = 2 + (int)sizeof(apple_id) - 1; // page header + string (no NUL)
                int total = 4 + 8 + page_len; // header + block descriptor + page
                phase_data_in(scsi, total);
                memset(scsi->buf.data, 0, total);
                scsi->buf.data[0] = (uint8_t)(total - 1); // mode data length
                scsi->buf.data[3] = 8; // block descriptor length
                // Block descriptor: 512-byte blocks
                scsi->buf.data[5] = (blocks >> 16) & 0xFF;
                scsi->buf.data[6] = (blocks >> 8) & 0xFF;
                scsi->buf.data[7] = blocks & 0xFF;
                scsi->buf.data[9] = 0x00;
                scsi->buf.data[10] = 0x02;
                scsi->buf.data[11] = 0x00;
                // Page 0x30 header
                scsi->buf.data[12] = 0x30; // page code
                scsi->buf.data[13] = (uint8_t)(sizeof(apple_id) - 1); // page length
                // "APPLE COMPUTER, INC." payload
                memcpy(scsi->buf.data + 14, apple_id, sizeof(apple_id) - 1);
            } else {
                // Default: return minimal mode sense header with block descriptor
                phase_data_in(scsi, 12);
                memset(scsi->buf.data, 0, 12);
                scsi->buf.data[0] = 11; // mode data length (excluding itself)
                scsi->buf.data[3] = 8; // block descriptor length
                // Block descriptor: 512-byte blocks
                scsi->buf.data[5] = (blocks >> 16) & 0xFF;
                scsi->buf.data[6] = (blocks >> 8) & 0xFF;
                scsi->buf.data[7] = blocks & 0xFF;
                scsi->buf.data[9] = 0x00;
                scsi->buf.data[10] = 0x02;
                scsi->buf.data[11] = 0x00;
            }
        }
        break;
    }

    case CMD_START_STOP_UNIT:
        if (scsi->devices[target].type == scsi_dev_cdrom)
            scsi_cdrom_start_stop_unit(scsi);
        else
            phase_status(scsi, STATUS_GOOD);
        break;

    case CMD_PREVENT_ALLOW:
        if (scsi->devices[target].type == scsi_dev_cdrom)
            scsi_cdrom_prevent_allow(scsi);
        else
            phase_status(scsi, STATUS_GOOD);
        break;

    case CMD_READ_CAPACITY: {

        // PMI=1 (byte 8 bit 0) asks for the last block before a performance
        // discontinuity; for a flat disk image that's the device's last LBA,
        // identical to PMI=0.  Don't assert on guest-supplied PMI — a
        // well-formed initiator may legitimately set it.
        image_t *image = scsi->devices[target].image;
        uint16_t blk_sz = scsi->devices[target].block_size;
        size_t sz = disk_size(image) / blk_sz;

        phase_data_in(scsi, 8);
        *(uint32_t *)(scsi->buf.data) = BE32((uint32_t)sz - 1);
        *(uint32_t *)(scsi->buf.data + 4) = BE32((uint32_t)blk_sz);

        break;
    }

    case CMD_VERIFY:
        // Verify data on disc — no-op (always succeeds)
        LOG(1, "command: VERIFY");
        phase_status(scsi, STATUS_GOOD);
        break;

    case CMD_READ_TOC:
        if (scsi->devices[target].type == scsi_dev_cdrom)
            scsi_cdrom_read_toc(scsi);
        else
            scsi_check_condition(scsi, SENSE_ILLEGAL_REQUEST, ASC_INVALID_OPCODE, 0x00);
        break;

    case CMD_READ_SUB_CHANNEL:
        if (scsi->devices[target].type == scsi_dev_cdrom)
            scsi_cdrom_read_sub_channel(scsi);
        else
            scsi_check_condition(scsi, SENSE_ILLEGAL_REQUEST, ASC_INVALID_OPCODE, 0x00);
        break;

    case CMD_READ_HEADER:
        if (scsi->devices[target].type == scsi_dev_cdrom)
            scsi_cdrom_read_header(scsi);
        else
            scsi_check_condition(scsi, SENSE_ILLEGAL_REQUEST, ASC_INVALID_OPCODE, 0x00);
        break;

    // Audio commands — stub: data-only disc, no audio support
    case CMD_PLAY_AUDIO_10:
    case CMD_PLAY_AUDIO_MSF:
    case CMD_PAUSE_RESUME:
        scsi_check_condition(scsi, SENSE_ILLEGAL_REQUEST, ASC_INCOMPATIBLE_MEDIUM, 0x00);
        break;

    // Sony vendor commands — data-only disc, return ILLEGAL REQUEST
    case CMD_SONY_READ_TOC:
        // Sony READ TOC: delegate to SCSI-2 READ TOC for CD-ROM
        if (scsi->devices[target].type == scsi_dev_cdrom)
            scsi_cdrom_read_toc(scsi);
        else
            scsi_check_condition(scsi, SENSE_ILLEGAL_REQUEST, ASC_INVALID_OPCODE, 0x00);
        break;

    case CMD_SONY_PLAYBACK_STATUS:
    case CMD_SONY_PAUSE:
    case CMD_SONY_PLAY_TRACK:
    case CMD_SONY_PLAY_MSF:
    case CMD_SONY_PLAY_AUDIO:
    case CMD_SONY_PLAYBACK_CTRL:
        // Audio vendor commands on data-only disc
        scsi_check_condition(scsi, SENSE_ILLEGAL_REQUEST, ASC_INCOMPATIBLE_MEDIUM, 0x00);
        break;

    default:
        // Unknown command: return CHECK CONDITION with ILLEGAL REQUEST
        scsi_check_condition(scsi, SENSE_ILLEGAL_REQUEST, ASC_INVALID_OPCODE, 0x00);
        break;
    }
}

// Finalize a SCSI command after data transfer is complete
static void command_complete(scsi_t *scsi) {
    int target = scsi->bus.target & 7;
    uint16_t blk_sz = scsi->devices[target].block_size;

    switch (scsi->cmd.opcode) {

    case CMD_WRITE:
    case CMD_WRITE_10: {
        assert(scsi->cmd.tl * blk_sz == scsi->buf.size);
        size_t device_bytes = disk_size(scsi->devices[target].image);
        assert(((size_t)scsi->cmd.lba + scsi->cmd.tl) * blk_sz <= device_bytes);

        disk_write_data(scsi->devices[target].image, (size_t)scsi->cmd.lba * blk_sz, scsi->buf.data,
                        (size_t)scsi->cmd.tl * blk_sz);

        scsi->buf.max = scsi->buf.size = 0;
    } break;

    case CMD_MODE_SELECT:
        // Process MODE SELECT data for CD-ROM (block size switching, etc.)
        if (scsi->devices[target].type == scsi_dev_cdrom) {
            scsi_cdrom_mode_select(scsi);
            return; // scsi_cdrom_mode_select calls phase_status itself
        }
        break;

    default:
        break;
    }

    phase_status(scsi, STATUS_GOOD);
}

// Perform SCSI bus reset: release all bus signals and return to bus-free state.
// On real hardware, a RST pulse forces all devices to release the bus and
// return to their power-on state.  We reset the controller registers and
// bus phase so the next arbitration/selection cycle starts cleanly.
// Per the NCR 5380 Design Manual, RST clears all registers and logic
// *except* the IRQ interrupt latch and the ASSERT RST bit in ICR.
static void scsi_reset(scsi_t *scsi) {
    scsi->bus.phase = scsi_bus_free;
    scsi->reg.csr = 0;
    scsi->reg.bsr = 0;
    scsi->reg.mr = 0;
    scsi->reg.tcr = 0;
    scsi->reg.odr = 0;
    scsi->reg.cdr = 0;
    scsi->buf.size = 0;
    scsi->end_of_dma = false;
    scsi->dma_write_armed = false;
    scsi->primer_held = false;
    // RST generates a non-maskable interrupt that survives the reset
    scsi->reg.bsr |= BSR_INT;
    // Flush the CDR pipeline so post-reset reads return $00
    scsi->cdr_pipeline[0] = scsi->cdr_pipeline[1] = scsi->cdr_pipeline[2] = 0;
    scsi->cdr_idx = 0;
    scsi_update_drq(scsi);
    scsi_update_irq(scsi);
}

// ============================================================================
// Memory Interface
// ============================================================================

// Write to the initiator command register
static void write_icr(scsi_t *scsi, uint8_t val) {
    uint8_t bits_set = val & (val ^ scsi->reg.icr);
    uint8_t bits_cleared = ~val & (val ^ scsi->reg.icr);

    SCSI_TRACE("write_icr: val=0x%02X old=0x%02X set=0x%02X clr=0x%02X phase=%d", val, scsi->reg.icr, bits_set,
               bits_cleared, scsi->bus.phase);

    scsi->reg.icr = val;

    // In loopback mode (passive terminator), skip bus state-machine
    // transitions — the diagnostic is testing register I/O, not device
    // interaction.  RST still resets the chip.
    // Per the NCR 5380 Design Manual, asserting RST holds the chip in
    // continuous reset (all registers cleared except IRQ and the RST bit
    // itself).  When RST is deasserted the chip emerges from reset with
    // all registers cleared — including the ICR output latch.
    if (scsi->loopback) {
        if (bits_set & ICR_RST)
            scsi_reset(scsi);
        if (bits_cleared & ICR_RST) {
            // RST deassertion: final chip reset, ICR latch cleared
            scsi_reset(scsi);
            scsi->reg.icr = 0;
        }
        return;
    }

    if (bits_set & ICR_RST) {
        SCSI_TRACE("write_icr: RST asserted -> scsi_reset");
        scsi_reset(scsi);
        return;
    }

    // if BSY is released in the selection phase, than it marks the end of selection
    if (bits_cleared & ICR_BSY && scsi->reg.icr & ICR_SEL) {

        assert(scsi->bus.phase == scsi_selection);

        // Non-arbitrated selection: A/UX's SCSI driver (and Apple's SCSI Manager
        // on real hardware) drives SEL without first setting MR_ARBITRATE, so
        // bus.initiator was never captured.  Mac hosts are wired to ID 7, so
        // default to 7 when arbitration was skipped.
        if (scsi->bus.initiator >= 8)
            scsi->bus.initiator = 7;

        // ODR will contain the "OR" of target and initiator ID
        scsi->bus.target = platform_ntz32(scsi->reg.odr & ~(1 << scsi->bus.initiator));

        // [6]: target will assert BSY - if no target, the bus will be free again
        if (!scsi->devices[scsi->bus.target & 7].image)
            phase_free(scsi);
        else
            phase_command(scsi);
    }

    // if ACK is reset
    if (bits_cleared & ICR_ACK) {

        if (scsi->bus.phase == scsi_command) {
            assert(scsi->buf.size <= cmd_size(scsi->buf.data[0]));
            if (scsi->buf.size == cmd_size(scsi->buf.data[0]))
                run_cmd(scsi);
            else
                scsi->reg.csr |= CSR_REQ;
        } else if (scsi->bus.phase == scsi_status)
            phase_message_in(scsi, MSG_CMD_COMPLETE);
        else if (scsi->bus.phase == scsi_message_in)
            phase_free(scsi);
        else if (scsi->bus.phase == scsi_message_out) {
            // Process the message byte received from the initiator.
            uint8_t msg = scsi->reg.odr;
            SCSI_TRACE("write_icr: MESSAGE OUT byte=0x%02X", msg);
            if (msg >= 0x80) {
                // IDENTIFY: LUN in bits 0-2, disconnect privilege in bit 6.
                // After IDENTIFY, resume the original phase's completion —
                // if we came from data_in/data_out with transfer done, go
                // to status; otherwise return to command phase.
                if (scsi->buf.size == 0 &&
                    (scsi->bus.saved_phase == scsi_data_in || scsi->bus.saved_phase == scsi_data_out))
                    phase_status(scsi, STATUS_GOOD);
                else
                    phase_status(scsi, STATUS_GOOD);
            } else if (msg == 0x06 || msg == 0x0C) {
                // ABORT or BUS DEVICE RESET: go bus free
                phase_free(scsi);
            } else {
                // Unhandled message — complete with GOOD status
                phase_status(scsi, STATUS_GOOD);
            }
        } else if (scsi->bus.phase == scsi_data_in) {
            if (scsi->buf.size == 0)
                phase_status(scsi, STATUS_GOOD);
            else
                scsi->reg.csr |= CSR_REQ;
        } else if (scsi->bus.phase == scsi_data_out) {
            // programmed I/O: check if transfer complete
            if (scsi->buf.size == scsi->buf.max)
                command_complete(scsi);
            else
                scsi->reg.csr |= CSR_REQ;
        }
        // ACK cleared on bus_free is harmless (e.g. SCSI diagnostics)
    }

    // if ACK is asserted
    if (bits_set & ICR_ACK) {

        // if a command - save the next byte in the buffer
        if (scsi->bus.phase == scsi_command) {
            assert(scsi->buf.size < scsi->buf.max);
            scsi->buf.data[scsi->buf.size++] = scsi->reg.odr;
        } else if (scsi->bus.phase == scsi_status)
            ;
        else if (scsi->bus.phase == scsi_message_in)
            ;
        else if (scsi->bus.phase == scsi_message_out)
            ; // message byte already latched in ODR
        else if (scsi->bus.phase == scsi_data_in)
            ; // byte already read via IDR
        else if (scsi->bus.phase == scsi_data_out) {
            // programmed I/O: save ODR byte to buffer
            assert(scsi->buf.size < scsi->buf.max);
            scsi->buf.data[scsi->buf.size++] = scsi->reg.odr;
        }
        // ACK on bus_free is harmless (e.g. SCSI diagnostics)

        // clear REQ
        scsi->reg.csr &= ~CSR_REQ;
    }

    if (bits_set & ICR_SEL) {
        SCSI_TRACE("write_icr: SEL asserted, phase=%d -> selection", scsi->bus.phase);
        phase_selection(scsi);
    }

    // ATN asserted: the initiator requests MESSAGE OUT phase.
    // On real hardware the target notices ATN during the next REQ/ACK
    // handshake and transitions at the first opportunity.  In the
    // emulator, if REQ is not currently asserted (data exhausted or
    // between handshakes) and the bus is in an information-transfer
    // phase, transition immediately — the target has no pending byte
    // to deliver first.
    if ((bits_set & ICR_ATN) && !(scsi->reg.csr & CSR_REQ)) {
        if (scsi->bus.phase == scsi_data_in || scsi->bus.phase == scsi_data_out || scsi->bus.phase == scsi_status ||
            scsi->bus.phase == scsi_message_in) {
            SCSI_TRACE("write_icr: ATN asserted, phase=%d -> message_out", scsi->bus.phase);
            phase_message_out(scsi);
        }
    }
}

// Write to the mode register
static void write_mr(scsi_t *scsi, uint8_t val) {
    uint8_t bits_set = val & (val ^ scsi->reg.mr);
    uint8_t bits_cleared = ~val & (val ^ scsi->reg.mr);

    scsi->reg.mr = val;

    // In loopback mode, just store the register — no arbitration or DMA
    if (scsi->loopback)
        return;

    // The ARBITRATE bit is set to start the arbitration process
    if (bits_set & MR_ARBITRATE) {

        SCSI_TRACE("write_mr: ARBITRATE set, phase=%d -> arbitration", scsi->bus.phase);
        phase_arbitration(scsi);

        // [1]: The results of the arbitration phase may be determined by reading the status bits LA and AIP
        scsi->reg.icr |= ICR_AIP;

        // Let's assume that we always win arbitration.
        // That is, LA is always cleard, and our own ID is always in the data register
        scsi->reg.icr &= ~ICR_LA;
        scsi->reg.cdr = scsi->reg.odr;
        scsi->bus.initiator = platform_ntz32(scsi->reg.odr);

        // Assert BSY on the bus after winning arbitration.  The NCR 5380
        // drives BSY when it becomes bus master; the ROM polls CSR_BSY
        // to detect arbitration completion.
        scsi->reg.csr |= CSR_BSY;
    }

    if (bits_cleared & MR_DMA) {
        scsi->reg.bsr &= ~BSR_DR;
        scsi->end_of_dma = false;
        scsi->dma_write_armed = false;
        // If data-in transfer completed (buffer drained) while DMA was
        // active, transition to status now.  The bus stayed in data_in
        // during DMA so the pseudo-DMA loop could see a clean BSR
        // (DRQ=0 / phase-mismatch=0 on read), but the actual phase
        // change is deferred until the host clears MR_DMA.  This
        // preserves Mac OS compatibility: Mac OS clears MR_DMA after
        // its byte-counting pseudo-DMA loop and expects the bus to
        // transition to status at that point (via TCR write or here).
        if (scsi->bus.phase == scsi_data_in && scsi->buf.size == 0)
            phase_status(scsi, STATUS_GOOD);
        scsi_update_drq(scsi);
        scsi_update_irq(scsi);
    } else if (bits_set & MR_DMA) {
        // MR.DMA just set — disarm the BLIND-write primer gate.  The host
        // arms the gate later by writing "Start DMA Send" (port 5).  Until
        // then, any ODR-alias writes are primer/setup writes that real
        // hardware would not transmit to the SCSI bus.
        scsi->dma_write_armed = false;

        // DMA mode can be set during data_in/data_out (normal), during
        // status/message_in (if command returned CHECK CONDITION before the
        // driver expected a data phase — the NCR 5380 signals this as a
        // phase mismatch IRQ so the driver can recover), or during command
        // (A/UX's SCSI driver uses pseudo-DMA to push command bytes).
        assert(scsi->bus.phase == scsi_command || scsi->bus.phase == scsi_data_in || scsi->bus.phase == scsi_data_out ||
               scsi->bus.phase == scsi_status || scsi->bus.phase == scsi_message_in);

        // if we're reading in data, and there is more in the buffer - then assert request signal
        if (scsi->bus.phase == scsi_data_in && scsi->buf.size != 0)
            scsi->reg.bsr |= BSR_DR;

        // if we're writing out data (command bytes or data-out), and there is
        // room in the buffer, assert request signal
        if ((scsi->bus.phase == scsi_data_out || scsi->bus.phase == scsi_command) && scsi->buf.size < scsi->buf.max)
            scsi->reg.bsr |= BSR_DR;

        // Status byte ready: A/UX's SPH_STAT reads the status byte via
        // pseudo-DMA.  On real NCR 5380 hardware, DRQ only asserts when
        // bus phase matches TCR — so A/UX programs TCR=status (0x03)
        // before enabling DMA, and phase_match is true.  Mac OS, if it
        // accidentally enables DMA while the bus is in status phase (e.g.
        // after a surprise CHECK CONDITION from a no-data command), has
        // TCR=data_in (0x01); phase_match is false, DRQ does not assert,
        // and the chip instead raises a phase-mismatch IRQ so the driver
        // can recover.
        if (scsi->bus.phase == scsi_status && scsi_phase_match(scsi))
            scsi->reg.bsr |= BSR_DR;

        scsi_update_drq(scsi);
        scsi_update_irq(scsi);
    }
}

// Read a byte from SCSI controller register
static uint8_t read_uint8(void *s, uint32_t addr) {
    scsi_t *scsi = (scsi_t *)s;

    // [5]: on 68000, reads are to even addresses (UDS). On 68030 (SE/30,
    // IIcx), the GLUE uses R/W directly and A0 is irrelevant for direction.
    // Register select always uses A4-A6.

    // [5] : a0-a2  are connected to a4-a6 of the cpu bus
    switch (addr >> 4 & 7) {
    case CDR:
    case IDR: // unclear if we need to make a distinction between CDR/IDR
        // Loopback: the CDR exhibits a 2-write pipeline delay — it reads
        // the bus state from before the second-to-last register write.
        // This models the NCR 5380's internal propagation delay: bus
        // driver outputs update 2 register-write cycles after the write.
        if (scsi->loopback) {
            uint8_t val = scsi->cdr_pipeline[(scsi->cdr_idx + 1) % 3];
            SCSI_TRACE("  SCSI RD CDR -> 0x%02X (pipeline)", val);
            return val;
        }
        if (scsi->bus.phase == scsi_data_in) {
            if (scsi->buf.size != 0) {
                scsi->reg.cdr = next_byte(scsi);
                // In DMA mode, deassert REQ after each byte to simulate
                // the real NCR 5380 handshake gap between bytes
                if (scsi->reg.mr & MR_DMA) {
                    scsi->reg.csr &= ~CSR_REQ;
                }
            } else
                phase_status(scsi, STATUS_GOOD);
        } else if (scsi->bus.phase == scsi_status && (scsi->reg.mr & MR_DMA) && scsi_phase_match(scsi)) {
            // Status byte consumed via pseudo-DMA read.  Only valid when
            // DRQ is asserted (phase_match true) — on real hardware the
            // pseudo-DMA ACK handshake only fires when the chip has
            // raised DRQ, which requires phase_match.  The ACK then
            // advances the target to message_in with COMMAND COMPLETE.
            // A/UX's SPH_STAT reaches this branch; Mac OS uses ICR/ACK
            // for status reads (MR_DMA=0) and never hits it.  A Mac OS
            // DMA-mode read during a surprise status phase is blocked by
            // the phase_match gate so the driver can recover via the
            // phase-mismatch IRQ.
            uint8_t val = scsi->reg.cdr;
            phase_message_in(scsi, MSG_CMD_COMPLETE);
            return val;
        }
        return scsi->reg.cdr;

    case ICR:
        SCSI_TRACE("  SCSI RD ICR -> 0x%02X", scsi->reg.icr);
        return scsi->reg.icr;

    case MR:
        SCSI_TRACE("  SCSI RD MR -> 0x%02X", scsi->reg.mr);
        return scsi->reg.mr;

    case TCR:
        SCSI_TRACE("  SCSI RD TCR -> 0x%02X", scsi->reg.tcr);
        return scsi->reg.tcr;

    case CSR:
        // Loopback: CSR reflects initiator-driven signals from ICR and
        // target-driven signals from TCR, as they appear on the bus
        if (scsi->loopback) {
            uint8_t val = scsi->reg.csr;
            // ICR-driven control signals reflected on the bus
            if (scsi->reg.icr & ICR_BSY)
                val |= CSR_BSY;
            if (scsi->reg.icr & ICR_SEL)
                val |= CSR_SEL;
            if (scsi->reg.icr & ICR_RST)
                val |= CSR_RST;
            // Target mode: TCR drives I/O, C/D, MSG, REQ onto the bus
            if (scsi->reg.mr & MR_TARGET) {
                if (scsi->reg.tcr & 0x01)
                    val |= CSR_IO;
                if (scsi->reg.tcr & 0x02)
                    val |= CSR_CD;
                if (scsi->reg.tcr & 0x04)
                    val |= CSR_MSG;
                if (scsi->reg.tcr & 0x08)
                    val |= CSR_REQ;
            }
            SCSI_TRACE("  SCSI RD CSR -> 0x%02X (icr=0x%02X tcr=0x%02X mr=0x%02X)", val, scsi->reg.icr, scsi->reg.tcr,
                       scsi->reg.mr);
            return val;
        }
        return scsi->reg.csr;

    case BSR:
        // Phase match is computed dynamically from CSR vs TCR
        if (scsi->loopback) {
            uint8_t val = scsi->reg.bsr;
            // ICR-driven signals readable via BSR
            if (scsi->reg.icr & ICR_ACK)
                val |= BSR_ACK;
            if (scsi->reg.icr & ICR_ATN)
                val |= BSR_ATN;
            val |= (scsi_phase_match(scsi) ? BSR_PM : 0);
            SCSI_TRACE("  SCSI RD BSR -> 0x%02X (icr=0x%02X bsr_stored=0x%02X pm=%d)", val, scsi->reg.icr,
                       scsi->reg.bsr, scsi_phase_match(scsi));
            return val;
        }
        return scsi->reg.bsr | (scsi_phase_match(scsi) ? BSR_PM : 0);

    case RESET:
        // Reading the Reset Parity/Interrupt register clears BSR bits
        // 5 (parity error), 4 (IRQ), and 2 (busy error)
        scsi->reg.bsr &= ~(0x04 | BSR_INT | 0x20);
        return 0xff;
    }

    assert(0);
    return 0;
}

// Read a 16-bit word from SCSI controller (not supported)
static uint16_t read_uint16(void *scsi, uint32_t addr) {
    assert(0);
    return 0;
}

// Read a 32-bit longword from SCSI controller (not supported)
static uint32_t read_uint32(void *scsi, uint32_t addr) {
    GS_ASSERT(0);
    return 0;
}

// Write a byte to SCSI controller register
static void write_uint8(void *s, uint32_t addr, uint8_t value) {
    scsi_t *scsi = (scsi_t *)s;

    if (scsi->loopback) {
        // Advance CDR pipeline: capture current bus state BEFORE this write
        // takes effect.  The NCR 5380 bus drivers update 2 write-cycles
        // after the register write, so CDR reads lag by 2 writes.
        scsi->cdr_pipeline[scsi->cdr_idx] = compute_loopback_cdr(scsi);
        scsi->cdr_idx = (scsi->cdr_idx + 1) % 3;

        static const char *regnames[] = {"ODR", "ICR", "MR", "TCR", "SER", "DMA", "TDMA", "IDMA"};
        SCSI_TRACE("  SCSI WR %s (reg %d) = 0x%02X", regnames[addr >> 4 & 7], (int)(addr >> 4 & 7), value);
    }

    // [5]: on 68000, writes are to odd addresses (LDS). On 68030 (SE/30,
    // IIcx), the GLUE uses R/W directly and A0 is irrelevant for direction.
    // Register select always uses A4-A6.

    // [5] : a0-a2  are connected to a4-a6 of the cpu bus
    switch (addr >> 4 & 7) {
    case ODR:
        if (addr & 0x200) {
            // Pseudo-DMA ODR alias: if the target expects a byte in the
            // current phase (command or data-out), push into the buffer.
            // In other phases, the write just latches into ODR — real
            // NCR 5380 doesn't gate ODR writes on phase.
            scsi->reg.odr = value;
            if (scsi->bus.phase == scsi_data_out || scsi->bus.phase == scsi_command) {
                // BLIND/DMA-write priming gate: when MR.DMA is active, only
                // consume the byte if the host has armed the transfer by
                // writing the "Start DMA Send" register (port 5, case DMA).
                // Real NCR 5380 hardware latches ODR on every write but only
                // transmits to the SCSI bus once the host has issued Start
                // DMA Send and the target has asserted REQ.  Pre-Start
                // writes — like A/UX's `CLR.B ([$5B20E,])` primer at PC
                // $1004B888 — are absorbed by the chip but never reach the
                // bus, so they must not land in our buf.data either.
                // When MR.DMA is OFF (Mac OS PIO command-byte path uses
                // ICR/ACK rather than pseudo-DMA), we always consume — the
                // ICR.ACK assertion provides the handshake.
                if ((scsi->reg.mr & MR_DMA) && !scsi->dma_write_armed)
                    break;
                // Primer-slot gate (data_out only): hold the first byte
                // and decide on the second.  If the held byte is $00 and
                // the second byte comes from a different PC, the held
                // byte is a kernel primer (A/UX's CLR.B at $1004B888) —
                // discard it and push only the second.  Otherwise the
                // held byte is real data — push held + push current.
                // See notes/60-aux3-volname-root-cause.md and
                // scsi_internal.h `primer_held`.
                if (scsi->bus.phase == scsi_data_out) {
                    cpu_t *cpu = system_cpu();
                    uint32_t pc = cpu ? cpu_get_pc(cpu) : 0;
                    if (!scsi->primer_held && scsi->buf.size == 0) {
                        scsi->primer_byte = value;
                        scsi->primer_pc = pc;
                        scsi->primer_held = true;
                        break;
                    }
                    if (scsi->primer_held) {
                        scsi->primer_held = false;
                        bool is_primer = (scsi->primer_byte == 0x00 && scsi->primer_pc != pc);
                        if (!is_primer) {
                            assert(scsi->buf.size < scsi->buf.max);
                            scsi->buf.data[scsi->buf.size++] = scsi->primer_byte;
                        }
                        // fall through to push current byte
                    }
                }
                assert(scsi->buf.size < scsi->buf.max);
                scsi->buf.data[scsi->buf.size++] = value;

                if (scsi->bus.phase == scsi_command) {
                    // Once we have all the command bytes, execute the command
                    if (scsi->buf.size == cmd_size(scsi->buf.data[0]))
                        run_cmd(scsi);
                } else if (scsi->buf.size == scsi->buf.max)
                    command_complete(scsi);
            }
        } else
            scsi->reg.odr = value;
        break;

    case ICR:
        write_icr(scsi, value);
        break;

    case MR:
        write_mr(scsi, value);
        break;

    case TCR:
        scsi->reg.tcr = value;
        // boot code seems to read only 256 bytes of block 0 and 1 (not a full block),
        // and then jump directly to "status" by asserting C/D and I/O in TCR
        if (scsi->bus.phase == scsi_data_in && (value & 7) == 3)
            phase_status(scsi, STATUS_GOOD);
        // TCR change affects phase match, which may trigger/clear IRQ
        scsi_update_irq(scsi);
        break;

    case SER:
        scsi->reg.ser = value;
        break;

    case DMA:
        scsi->reg.bsr |= BSR_DR;
        // Start DMA Send (NCR 5380 §6.8.1): the host writes this register
        // to begin a pseudo-DMA send — until then, ODR-alias writes are
        // primer/setup writes that do not transfer.  Arm the gate.
        scsi->dma_write_armed = true;
        scsi_update_drq(scsi);
        break;

    case TDMA:
    case IDMA:
        break;

    default:
        assert(0);
        break;
    }
}

// Write a 16-bit word to SCSI controller (not supported)
static void write_uint16(void *scsi, uint32_t addr, uint16_t value) {
    assert(0);
}

// Write a 32-bit longword to SCSI controller (not supported)
static void write_uint32(void *scsi, uint32_t addr, uint32_t value) {
    assert(0);
}

// ============================================================================
// Lifecycle: Constructor
// ============================================================================

// Add a SCSI device to the bus at the specified SCSI ID
void scsi_add_device(scsi_t *restrict scsi, int scsi_id, const char *vendor, const char *product, const char *revision,
                     image_t *image, enum scsi_device_type type, uint16_t block_size, bool read_only) {
    assert(scsi_id < 7);

    scsi->devices[scsi_id].image = image;
    scsi->devices[scsi_id].type = type;
    scsi->devices[scsi_id].block_size = block_size;
    scsi->devices[scsi_id].read_only = read_only;
    scsi->devices[scsi_id].medium_present = (image != NULL);
    scsi->devices[scsi_id].prevent_removal = false;
    memset(&scsi->devices[scsi_id].sense, 0, sizeof(scsi->devices[scsi_id].sense));

    // Left-justify and space-pad per SCSI spec (vendor=8, product=16 bytes)
    snprintf((char *)scsi->devices[scsi_id].vendor_id, 9, "%-8s", vendor);
    snprintf((char *)scsi->devices[scsi_id].product_id, 17, "%-16s", product);
    if (revision)
        snprintf((char *)scsi->devices[scsi_id].revision, sizeof(scsi->devices[scsi_id].revision), "%s", revision);
    else
        memset(scsi->devices[scsi_id].revision, ' ', 4);

    // CD-ROM attach triggers UNIT ATTENTION (media changed)
    if (type == scsi_dev_cdrom && image != NULL)
        scsi->devices[scsi_id].unit_attention = true;
}

// Initialize the SCSI controller and optionally restore from checkpoint
scsi_t *scsi_init(memory_map_t *map, checkpoint_t *checkpoint) {
    scsi_t *scsi = (scsi_t *)malloc(sizeof(scsi_t));
    if (scsi == NULL)
        return NULL;

    memset(scsi, 0, sizeof(scsi_t));

    scsi->memory_interface.read_uint8 = &read_uint8;
    scsi->memory_interface.read_uint16 = &read_uint16;
    scsi->memory_interface.read_uint32 = &read_uint32;

    scsi->memory_interface.write_uint8 = &write_uint8;
    scsi->memory_interface.write_uint16 = &write_uint16;
    scsi->memory_interface.write_uint32 = &write_uint32;

    // Register with memory map if provided (NULL = machine handles registration)
    if (map)
        memory_map_add(map, 0x00500000, 0x00100000, "scsi", &scsi->memory_interface, scsi);

    scsi->bus.phase = scsi_bus_free;

    scsi->buf.data = malloc(BUF_LIMIT);
    scsi->buf.max = MAX_CMD_SIZE;
    scsi->bus.initiator = INT_MAX;

    // If checkpoint provided, restore plain-data portion first
    if (checkpoint) {
        size_t data_size = offsetof(scsi_t, devices);
        system_read_checkpoint_data(checkpoint, scsi, data_size);

        // Restore device vendor/product strings, extended fields, and image filename
        for (int i = 0; i < 8; i++) {
            // vendor_id and product_id are fixed-size arrays inside devices[i]
            system_read_checkpoint_data(checkpoint, scsi->devices[i].vendor_id, sizeof(scsi->devices[i].vendor_id));
            system_read_checkpoint_data(checkpoint, scsi->devices[i].product_id, sizeof(scsi->devices[i].product_id));

            // Extended fields: revision, type, block_size, read_only, unit_attention
            system_read_checkpoint_data(checkpoint, scsi->devices[i].revision, sizeof(scsi->devices[i].revision));
            system_read_checkpoint_data(checkpoint, &scsi->devices[i].type, sizeof(scsi->devices[i].type));
            system_read_checkpoint_data(checkpoint, &scsi->devices[i].block_size, sizeof(scsi->devices[i].block_size));
            system_read_checkpoint_data(checkpoint, &scsi->devices[i].read_only, sizeof(scsi->devices[i].read_only));
            system_read_checkpoint_data(checkpoint, &scsi->devices[i].unit_attention,
                                        sizeof(scsi->devices[i].unit_attention));

            // Backward compat: default HD values for old checkpoints missing these fields
            if (scsi->devices[i].block_size == 0)
                scsi->devices[i].block_size = 512;
            if (scsi->devices[i].type == scsi_dev_none && scsi->devices[i].vendor_id[0] != 0)
                scsi->devices[i].type = scsi_dev_hd;

            uint32_t len = 0;
            system_read_checkpoint_data(checkpoint, &len, sizeof(len));
            scsi->devices[i].image = NULL;
            scsi->devices[i].medium_present = false;
            if (len > 0) {
                char *name = (char *)malloc(len);
                if (name) {
                    system_read_checkpoint_data(checkpoint, name, len);
                    image_t *img = setup_get_image_by_filename(name);
                    if (img) {
                        scsi->devices[i].image = img;
                        scsi->devices[i].medium_present = true;
                    }
                    free(name);
                } else {
                    // consume bytes to keep stream aligned
                    char tmp;
                    for (uint32_t k = 0; k < len; ++k)
                        system_read_checkpoint_data(checkpoint, &tmp, 1);
                }
            }
        }

        // Restore buffer contents (if any). The buf.data has been allocated above. Restore its max/size first.
        system_read_checkpoint_data(checkpoint, &scsi->buf.max, sizeof(scsi->buf.max));
        system_read_checkpoint_data(checkpoint, &scsi->buf.size, sizeof(scsi->buf.size));
        if (scsi->buf.size && scsi->buf.data) {
            size_t to_read = scsi->buf.size;
            if (to_read > BUF_LIMIT)
                to_read = BUF_LIMIT;
            system_read_checkpoint_data(checkpoint, scsi->buf.data, to_read);
        }
    }

    // Object-tree binding — instance_data is the scsi itself, with bus
    // and devices children plus the per-slot device entries that the
    // indexed-child get() returns on demand.
    scsi->object = object_new(&scsi_class, scsi, "scsi");
    if (scsi->object) {
        object_attach(object_root(), scsi->object);
        scsi->bus_object = object_new(&scsi_bus_class, scsi, "bus");
        if (scsi->bus_object)
            object_attach(scsi->object, scsi->bus_object);
        scsi->devices_object = object_new(&scsi_devices_collection_class, scsi, "devices");
        if (scsi->devices_object)
            object_attach(scsi->object, scsi->devices_object);
        for (int i = 0; i < 8; i++) {
            scsi->device_links[i].scsi = scsi;
            scsi->device_links[i].slot = i;
            scsi->device_objects[i] = object_new(&scsi_device_class, &scsi->device_links[i], NULL);
        }
    }

    return scsi;
}

// ============================================================================
// Accessors
// ============================================================================

// Return the SCSI memory-mapped I/O interface for machine-level address decode
const memory_interface_t *scsi_get_memory_interface(scsi_t *scsi) {
    return &scsi->memory_interface;
}

// Connect SCSI interrupt outputs to VIA2 (CB2 = /IRQ, CA2 = /DRQ)
void scsi_set_via(scsi_t *scsi, via_t *via) {
    scsi->via = via;
}

// Enable or disable loopback mode (passive SCSI terminator / test card)
void scsi_set_loopback(scsi_t *scsi, bool enable) {
    scsi->loopback = enable;
}

// Query whether loopback mode is active
bool scsi_get_loopback(scsi_t *scsi) {
    return scsi->loopback;
}

// Eject the medium currently in the SCSI device at `id` (0..6).  Mirrors
// the START/STOP UNIT eject path: clear the medium pointer, set the
// medium-not-present unit attention so the host sees a fresh transition.
// Returns 1 on success, 0 if the slot was already empty, -1 on error.
int scsi_eject_device(scsi_t *scsi, int id) {
    if (!scsi || id < 0 || id > 6)
        return -1;
    if (!scsi->devices[id].image && !scsi->devices[id].medium_present)
        return 0;
    scsi->devices[id].medium_present = false;
    scsi->devices[id].image = NULL;
    scsi->devices[id].unit_attention = true;
    scsi_set_sense(scsi, id, SENSE_UNIT_ATTENTION, ASC_MEDIUM_NOT_PRESENT, 0x00);
    return 1;
}

// === M7d — read-only views for the object model =============================

int scsi_get_bus_phase(const scsi_t *scsi) {
    return scsi ? (int)scsi->bus.phase : 0;
}
int scsi_get_bus_target(const scsi_t *scsi) {
    return scsi ? scsi->bus.target : -1;
}
int scsi_get_bus_initiator(const scsi_t *scsi) {
    return scsi ? scsi->bus.initiator : -1;
}

int scsi_device_type(const scsi_t *scsi, unsigned which) {
    if (!scsi || which > 7)
        return 0;
    return (int)scsi->devices[which].type;
}
bool scsi_device_present(const scsi_t *scsi, unsigned which) {
    return scsi_device_type(scsi, which) != scsi_dev_none;
}
bool scsi_device_read_only(const scsi_t *scsi, unsigned which) {
    if (!scsi || which > 7)
        return false;
    return scsi->devices[which].read_only;
}
bool scsi_device_medium_present(const scsi_t *scsi, unsigned which) {
    if (!scsi || which > 7)
        return false;
    return scsi->devices[which].medium_present;
}
uint16_t scsi_device_block_size(const scsi_t *scsi, unsigned which) {
    if (!scsi || which > 7)
        return 0;
    return scsi->devices[which].block_size;
}
const char *scsi_device_vendor(const scsi_t *scsi, unsigned which) {
    if (!scsi || which > 7 || scsi->devices[which].type == scsi_dev_none)
        return NULL;
    return (const char *)scsi->devices[which].vendor_id;
}
const char *scsi_device_product(const scsi_t *scsi, unsigned which) {
    if (!scsi || which > 7 || scsi->devices[which].type == scsi_dev_none)
        return NULL;
    return (const char *)scsi->devices[which].product_id;
}
const char *scsi_device_revision(const scsi_t *scsi, unsigned which) {
    if (!scsi || which > 7 || scsi->devices[which].type == scsi_dev_none)
        return NULL;
    return (const char *)scsi->devices[which].revision;
}

struct image *scsi_device_image(const scsi_t *scsi, unsigned which) {
    if (!scsi || which > 7 || scsi->devices[which].type == scsi_dev_none)
        return NULL;
    if (!scsi->devices[which].medium_present)
        return NULL;
    return scsi->devices[which].image;
}

// ============================================================================
// Lifecycle: Destructor
// ============================================================================

// Free resources associated with SCSI controller
void scsi_delete(scsi_t *scsi) {
    if (!scsi)
        return;
    // Tear down per-slot entry objects (never attached to the tree),
    // then the named children, then the top-level node.
    for (int i = 0; i < 8; i++) {
        if (scsi->device_objects[i]) {
            object_delete(scsi->device_objects[i]);
            scsi->device_objects[i] = NULL;
        }
    }
    if (scsi->devices_object) {
        object_detach(scsi->devices_object);
        object_delete(scsi->devices_object);
        scsi->devices_object = NULL;
    }
    if (scsi->bus_object) {
        object_detach(scsi->bus_object);
        object_delete(scsi->bus_object);
        scsi->bus_object = NULL;
    }
    if (scsi->object) {
        object_detach(scsi->object);
        object_delete(scsi->object);
        scsi->object = NULL;
    }
    if (scsi->buf.data) {
        free(scsi->buf.data);
        scsi->buf.data = NULL;
    }
    free(scsi);
}

// ============================================================================
// Lifecycle: Checkpointing
// ============================================================================

// Save SCSI controller state to checkpoint
void scsi_checkpoint(scsi_t *restrict scsi, checkpoint_t *checkpoint) {
    if (!scsi || !checkpoint)
        return;

    // Save contiguous plain-data portion up to devices (devices contains pointers)
    size_t data_size = offsetof(scsi_t, devices);
    system_write_checkpoint_data(checkpoint, scsi, data_size);

    // Save vendor/product strings, extended fields, and per-device image filename
    for (int i = 0; i < 8; i++) {
        system_write_checkpoint_data(checkpoint, scsi->devices[i].vendor_id, sizeof(scsi->devices[i].vendor_id));
        system_write_checkpoint_data(checkpoint, scsi->devices[i].product_id, sizeof(scsi->devices[i].product_id));

        // Extended fields: revision, type, block_size, read_only, unit_attention
        system_write_checkpoint_data(checkpoint, scsi->devices[i].revision, sizeof(scsi->devices[i].revision));
        system_write_checkpoint_data(checkpoint, &scsi->devices[i].type, sizeof(scsi->devices[i].type));
        system_write_checkpoint_data(checkpoint, &scsi->devices[i].block_size, sizeof(scsi->devices[i].block_size));
        system_write_checkpoint_data(checkpoint, &scsi->devices[i].read_only, sizeof(scsi->devices[i].read_only));
        system_write_checkpoint_data(checkpoint, &scsi->devices[i].unit_attention,
                                     sizeof(scsi->devices[i].unit_attention));

        const char *name = scsi->devices[i].image ? image_get_filename(scsi->devices[i].image) : NULL;
        uint32_t len = 0;
        if (name && *name)
            len = (uint32_t)strlen(name) + 1; // include NUL
        system_write_checkpoint_data(checkpoint, &len, sizeof(len));
        if (len) {
            system_write_checkpoint_data(checkpoint, name, len);
        }
    }

    // Save buf metadata and contents
    system_write_checkpoint_data(checkpoint, &scsi->buf.max, sizeof(scsi->buf.max));
    system_write_checkpoint_data(checkpoint, &scsi->buf.size, sizeof(scsi->buf.size));
    if (scsi->buf.size && scsi->buf.data) {
        size_t to_write = scsi->buf.size;
        if (to_write > BUF_LIMIT)
            to_write = BUF_LIMIT;
        system_write_checkpoint_data(checkpoint, scsi->buf.data, to_write);
    }
}

// === Object-model class descriptors =========================================
//
// `scsi` exposes:
//   - `loopback` (R/W bool) — wraps scsi_get/set_loopback
//   - `bus` named child with `phase` (V_ENUM), `target`, `initiator`
//   - `devices` indexed children — sparse stable indices = SCSI ID 0..7
//     (proposal §5.4: "scsi.devices (indexed; each device exposes id,
//     type, image (path attribute), methods eject(), insert)")
//
// Empty SCSI IDs are holes in the indexed collection: `count()`
// returns the number of populated slots, `next()` skips empties.
//
// instance_data on the top-level scsi node, the bus child, and the
// devices collection is the scsi_t* itself. Per-device entry objects
// carry a pointer into scsi->device_links[slot] so accessors can
// recover (scsi, slot) in one indirection.

static scsi_t *scsi_self_from(struct object *self) {
    return (scsi_t *)object_data(self);
}

// --- Per-device entry class ------------------------------------------------

static scsi_t *scsi_dev_scsi(struct object *self, unsigned *slot_out) {
    // instance_data is a pointer into scsi->device_links[i]; recover
    // both the controller and the slot index from it.
    scsi_device_link_t *link = (scsi_device_link_t *)object_data(self);
    if (!link) {
        *slot_out = 0;
        return NULL;
    }
    *slot_out = (unsigned)link->slot;
    return link->scsi;
}

static value_t scsi_dev_attr_id(struct object *self, const member_t *m) {
    (void)m;
    unsigned slot = 0;
    (void)scsi_dev_scsi(self, &slot);
    return val_int((int)slot);
}

static const char *const SCSI_DEV_TYPE_NAMES[] = {"none", "hd", "cdrom"};

static value_t scsi_dev_attr_type(struct object *self, const member_t *m) {
    (void)m;
    unsigned slot = 0;
    scsi_t *scsi = scsi_dev_scsi(self, &slot);
    int t = scsi_device_type(scsi, slot);
    if (t < 0 || t > 2)
        t = 0;
    return val_enum(t, SCSI_DEV_TYPE_NAMES, 3);
}

static value_t scsi_dev_attr_vendor(struct object *self, const member_t *m) {
    (void)m;
    unsigned slot = 0;
    scsi_t *scsi = scsi_dev_scsi(self, &slot);
    const char *s = scsi_device_vendor(scsi, slot);
    return val_str(s ? s : "");
}
static value_t scsi_dev_attr_product(struct object *self, const member_t *m) {
    (void)m;
    unsigned slot = 0;
    scsi_t *scsi = scsi_dev_scsi(self, &slot);
    const char *s = scsi_device_product(scsi, slot);
    return val_str(s ? s : "");
}
static value_t scsi_dev_attr_revision(struct object *self, const member_t *m) {
    (void)m;
    unsigned slot = 0;
    scsi_t *scsi = scsi_dev_scsi(self, &slot);
    const char *s = scsi_device_revision(scsi, slot);
    return val_str(s ? s : "");
}
static value_t scsi_dev_attr_block_size(struct object *self, const member_t *m) {
    (void)m;
    unsigned slot = 0;
    scsi_t *scsi = scsi_dev_scsi(self, &slot);
    return val_uint(2, scsi_device_block_size(scsi, slot));
}
static value_t scsi_dev_attr_read_only(struct object *self, const member_t *m) {
    (void)m;
    unsigned slot = 0;
    scsi_t *scsi = scsi_dev_scsi(self, &slot);
    return val_bool(scsi_device_read_only(scsi, slot));
}
static value_t scsi_dev_attr_medium_present(struct object *self, const member_t *m) {
    (void)m;
    unsigned slot = 0;
    scsi_t *scsi = scsi_dev_scsi(self, &slot);
    return val_bool(scsi_device_medium_present(scsi, slot));
}

// `eject()` — eject the medium from this device.
static value_t scsi_dev_method_eject(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    (void)argc;
    (void)argv;
    unsigned slot = 0;
    scsi_t *scsi = scsi_dev_scsi(self, &slot);
    if (!scsi)
        return val_err("scsi.devices.N.eject: scsi controller not available");
    int rc = scsi_eject_device(scsi, (int)slot);
    if (rc < 0)
        return val_err("scsi.devices.N.eject: invalid SCSI ID %u", slot);
    if (rc == 0)
        printf("scsi.devices[%u].eject: no medium present\n", slot);
    else
        printf("scsi.devices[%u].eject: ejected\n", slot);
    return val_bool(rc != 0);
}

// `insert(path)` — mount a CD-ROM image into this slot.
static value_t scsi_dev_method_insert(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING || !argv[0].s)
        return val_err("scsi.devices.N.insert: expected (path)");
    unsigned slot = 0;
    if (!scsi_dev_scsi(self, &slot))
        return val_err("scsi.devices.N.insert: scsi controller not available");
    if (!global_emulator)
        return val_err("scsi.devices.N.insert: emulator not initialised");
    add_scsi_cdrom(global_emulator, argv[0].s, (int)slot);
    return val_bool(true);
}

// `info()` — human-readable summary of the device contents.
static value_t scsi_dev_method_info(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)m;
    (void)argc;
    (void)argv;
    unsigned slot = 0;
    scsi_t *scsi = scsi_dev_scsi(self, &slot);
    if (!scsi)
        return val_err("scsi.devices.N.info: scsi controller not available");
    if (!scsi_device_present(scsi, slot)) {
        printf("scsi.devices[%u]: no device present\n", slot);
        return val_bool(false);
    }
    image_t *img = scsi_device_image(scsi, slot);
    if (!img) {
        printf("scsi.devices[%u]: device present, no medium\n", slot);
        return val_bool(true);
    }
    const char *fname = image_get_filename(img);
    size_t sz = disk_size(img);
    double size_mb = (double)sz / (1024.0 * 1024.0);
    printf("scsi.devices[%u]: %.1f MB — %s\n", slot, size_mb, fname ? fname : "(unknown)");
    return val_bool(true);
}

static const arg_decl_t scsi_dev_insert_args[] = {
    {.name = "path", .kind = V_STRING, .doc = "Host path or storage URI of the image to mount"},
};

static const member_t scsi_device_members[] = {
    {.kind = M_ATTR,   .name = "id",   .flags = VAL_RO,                       .attr = {.type = V_INT, .get = scsi_dev_attr_id, .set = NULL}   },
    {.kind = M_ATTR,   .name = "type", .flags = VAL_RO,                       .attr = {.type = V_ENUM, .get = scsi_dev_attr_type, .set = NULL}},
    {.kind = M_ATTR,
     .name = "vendor",
     .flags = VAL_RO,
     .attr = {.type = V_STRING, .get = scsi_dev_attr_vendor, .set = NULL}                                                                     },
    {.kind = M_ATTR,
     .name = "product",
     .flags = VAL_RO,
     .attr = {.type = V_STRING, .get = scsi_dev_attr_product, .set = NULL}                                                                    },
    {.kind = M_ATTR,
     .name = "revision",
     .flags = VAL_RO,
     .attr = {.type = V_STRING, .get = scsi_dev_attr_revision, .set = NULL}                                                                   },
    {.kind = M_ATTR,
     .name = "block_size",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = scsi_dev_attr_block_size, .set = NULL}                                                                   },
    {.kind = M_ATTR,
     .name = "read_only",
     .flags = VAL_RO,
     .attr = {.type = V_BOOL, .get = scsi_dev_attr_read_only, .set = NULL}                                                                    },
    {.kind = M_ATTR,
     .name = "medium_present",
     .flags = VAL_RO,
     .attr = {.type = V_BOOL, .get = scsi_dev_attr_medium_present, .set = NULL}                                                               },
    {.kind = M_METHOD,
     .name = "eject",
     .doc = "Eject the medium (CD-ROMs leave the slot attached, HDs detach)",
     .method = {.args = NULL, .nargs = 0, .result = V_BOOL, .fn = scsi_dev_method_eject}                                                      },
    {.kind = M_METHOD,
     .name = "insert",
     .doc = "Mount a CD-ROM image into this slot",
     .method = {.args = scsi_dev_insert_args, .nargs = 1, .result = V_BOOL, .fn = scsi_dev_method_insert}                                     },
    {.kind = M_METHOD,
     .name = "info",
     .doc = "Print a human-readable summary of the device contents",
     .method = {.args = NULL, .nargs = 0, .result = V_BOOL, .fn = scsi_dev_method_info}                                                       },
};

const class_desc_t scsi_device_class = {
    .name = "scsi_device",
    .members = scsi_device_members,
    .n_members = sizeof(scsi_device_members) / sizeof(scsi_device_members[0]),
};

// --- bus child class -------------------------------------------------------

static const char *const SCSI_PHASE_NAMES[] = {
    "bus_free", "arbitration", "selection", "reselection", "command",
    "data_in",  "data_out",    "status",    "message_in",  "message_out",
};

static value_t scsi_bus_attr_phase(struct object *self, const member_t *m) {
    (void)m;
    int p = scsi_get_bus_phase((scsi_t *)object_data(self));
    if (p < 0 || p > 9)
        p = 0;
    return val_enum(p, SCSI_PHASE_NAMES, 10);
}
static value_t scsi_bus_attr_target(struct object *self, const member_t *m) {
    (void)m;
    return val_int(scsi_get_bus_target((scsi_t *)object_data(self)));
}
static value_t scsi_bus_attr_initiator(struct object *self, const member_t *m) {
    (void)m;
    return val_int(scsi_get_bus_initiator((scsi_t *)object_data(self)));
}

static const member_t scsi_bus_members[] = {
    {.kind = M_ATTR,
     .name = "phase",
     .flags = VAL_RO,
     .attr = {.type = V_ENUM, .get = scsi_bus_attr_phase, .set = NULL}   },
    {.kind = M_ATTR,
     .name = "target",
     .flags = VAL_RO,
     .attr = {.type = V_INT, .get = scsi_bus_attr_target, .set = NULL}   },
    {.kind = M_ATTR,
     .name = "initiator",
     .flags = VAL_RO,
     .attr = {.type = V_INT, .get = scsi_bus_attr_initiator, .set = NULL}},
};
const class_desc_t scsi_bus_class = {
    .name = "scsi_bus",
    .members = scsi_bus_members,
    .n_members = sizeof(scsi_bus_members) / sizeof(scsi_bus_members[0]),
};

// --- devices collection: indexed children -----------------------------------
//
// Empty SCSI IDs are holes in the indexed collection: `count()`
// returns the number of populated slots, `next()` skips empties.
// Per-entry objects live on the scsi_t (scsi->device_objects[]) and
// are created/destroyed alongside the controller.

static struct object *scsi_devices_get(struct object *self, int index) {
    scsi_t *scsi = (scsi_t *)object_data(self);
    if (!scsi || index < 0 || index > 7)
        return NULL;
    if (!scsi_device_present(scsi, (unsigned)index))
        return NULL;
    return scsi->device_objects[index];
}
static int scsi_devices_count(struct object *self) {
    scsi_t *scsi = (scsi_t *)object_data(self);
    if (!scsi)
        return 0;
    int n = 0;
    for (int i = 0; i < 8; i++)
        if (scsi_device_present(scsi, (unsigned)i))
            n++;
    return n;
}
static int scsi_devices_next(struct object *self, int prev_index) {
    scsi_t *scsi = (scsi_t *)object_data(self);
    if (!scsi)
        return -1;
    for (int i = prev_index + 1; i < 8; i++)
        if (scsi_device_present(scsi, (unsigned)i))
            return i;
    return -1;
}

static const member_t scsi_devices_collection_members[] = {
    {.kind = M_CHILD,
     .name = "entries",
     .child = {.cls = &scsi_device_class,
               .indexed = true,
               .get = scsi_devices_get,
               .count = scsi_devices_count,
               .next = scsi_devices_next,
               .lookup = NULL}},
};
const class_desc_t scsi_devices_collection_class = {
    .name = "scsi_devices",
    .members = scsi_devices_collection_members,
    .n_members = 1,
};

// --- top-level scsi class ---------------------------------------------------

static value_t scsi_attr_loopback_get(struct object *self, const member_t *m) {
    (void)m;
    return val_bool(scsi_get_loopback(scsi_self_from(self)));
}
static value_t scsi_attr_loopback_set(struct object *self, const member_t *m, value_t in) {
    (void)m;
    scsi_t *scsi = scsi_self_from(self);
    if (!scsi) {
        value_free(&in);
        return val_err("scsi not available");
    }
    bool b = val_as_bool(&in);
    value_free(&in);
    scsi_set_loopback(scsi, b);
    return val_none();
}

// `scsi.identify_hd(path)` — true if the file looks like a SCSI HD image:
// it opens, isn't a floppy-sized image, and has a non-zero size. Prints a
// closest-match drive model line for diagnostic context.
static value_t scsi_method_identify_hd(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING || !argv[0].s)
        return val_err("scsi.identify_hd: expected (path)");
    const char *path = argv[0].s;
    image_t *img = image_open_readonly(path);
    if (!img) {
        printf("invalid SCSI HD image: cannot open %s\n", path);
        return val_bool(false);
    }
    if (img->type == image_fd_ss || img->type == image_fd_ds || img->type == image_fd_hd) {
        printf("invalid SCSI HD image: size matches floppy (%zu bytes)\n", img->raw_size);
        image_close(img);
        return val_bool(false);
    }
    size_t sz = img->raw_size;
    const struct drive_model *best = drive_catalog_find_closest(sz);
    if (sz == best->size)
        printf("valid SCSI HD image: %zu bytes, matches %s %s\n", sz, best->vendor, best->product);
    else
        printf("valid SCSI HD image: %zu bytes, nearest model %s %s\n", sz, best->vendor, best->product);
    image_close(img);
    return val_bool(true);
}

// `scsi.identify_cdrom(path)` — true if the file is a recognised CD-ROM
// image (ISO 9660, HFS, or Apple Partition Map). Prints a one-line
// diagnostic describing what was matched.
static value_t scsi_method_identify_cdrom(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 1 || argv[0].kind != V_STRING || !argv[0].s)
        return val_err("scsi.identify_cdrom: expected (path)");
    const char *path = argv[0].s;
    image_t *img = image_open_readonly(path);
    if (!img) {
        printf("invalid CD-ROM image: cannot open %s\n", path);
        return val_bool(false);
    }
    if (img->type == image_fd_ss || img->type == image_fd_ds || img->type == image_fd_hd) {
        printf("invalid CD-ROM image: floppy-sized (%zu bytes)\n", img->raw_size);
        image_close(img);
        return val_bool(false);
    }
    bool is_iso = false, is_hfs = false, is_apm = false;
    size_t sz = disk_size(img);
    uint8_t sector[512];
    if (sz >= 33280) {
        disk_read_data(img, 32768, sector, 512);
        if (memcmp(sector + 1, "CD001", 5) == 0)
            is_iso = true;
    }
    if (sz >= 1536) {
        disk_read_data(img, 1024, sector, 512);
        if (sector[0] == 0x42 && sector[1] == 0x44)
            is_hfs = true;
    }
    if (sz >= 1024) {
        disk_read_data(img, 0, sector, 512);
        bool has_ddm = (sector[0] == 0x45 && sector[1] == 0x52);
        disk_read_data(img, 512, sector, 512);
        bool has_pm = (sector[0] == 0x50 && sector[1] == 0x4D);
        if (has_ddm && has_pm)
            is_apm = true;
    }
    double size_mb = (double)sz / (1024.0 * 1024.0);
    if (is_iso && is_hfs)
        printf("valid CD-ROM image: %.1f MB, ISO 9660 + HFS hybrid\n", size_mb);
    else if (is_iso)
        printf("valid CD-ROM image: %.1f MB, ISO 9660\n", size_mb);
    else if (is_hfs)
        printf("valid CD-ROM image: %.1f MB, HFS\n", size_mb);
    else if (is_apm)
        printf("valid CD-ROM image: %.1f MB, Apple Partition Map\n", size_mb);
    else {
        printf("invalid CD-ROM image: no ISO 9660, HFS, or Apple Partition Map detected\n");
        image_close(img);
        return val_bool(false);
    }
    image_close(img);
    return val_bool(true);
}

// `scsi.attach_hd(path, id)` — attach a hard-disk image at the given SCSI id.
// Routes through shell_hd_argv so the legacy attach-image plumbing stays in
// one place.
static value_t scsi_method_attach_hd(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 2 || argv[0].kind != V_STRING || !argv[0].s)
        return val_err("scsi.attach_hd: expected (path, id)");
    bool ok = false;
    int64_t id = val_as_i64(&argv[1], &ok);
    if (!ok && argv[1].kind == V_UINT)
        id = (int64_t)argv[1].u;
    else if (!ok)
        return val_err("scsi.attach_hd: id must be an integer");
    if (id < 0 || id > 6)
        return val_err("scsi.attach_hd: id must be 0..6");
    char line[1024];
    int n = snprintf(line, sizeof(line), "hd attach \"%s\" %lld", argv[0].s, (long long)id);
    if (n < 0 || (size_t)n >= sizeof(line))
        return val_err("scsi.attach_hd: arguments too long");
    char *targv[16];
    int targc = tokenize(line, targv, 16);
    if (targc <= 0)
        return val_err("scsi.attach_hd: tokenisation failed");
    return val_bool(shell_hd_argv(targc, targv) == 0);
}

// `scsi.attach_cdrom(path, id)` — attach a CD-ROM image at the given SCSI id.
static value_t scsi_method_attach_cdrom(struct object *self, const member_t *m, int argc, const value_t *argv) {
    (void)self;
    (void)m;
    if (argc < 2 || argv[0].kind != V_STRING || !argv[0].s)
        return val_err("scsi.attach_cdrom: expected (path, id)");
    bool ok = false;
    int64_t id = val_as_i64(&argv[1], &ok);
    if (!ok && argv[1].kind == V_UINT)
        id = (int64_t)argv[1].u;
    else if (!ok)
        return val_err("scsi.attach_cdrom: id must be an integer");
    if (id < 0 || id > 6)
        return val_err("scsi.attach_cdrom: id must be 0..6");
    if (!global_emulator)
        return val_err("scsi.attach_cdrom: emulator not initialised");
    add_scsi_cdrom(global_emulator, argv[0].s, (int)id);
    return val_bool(true);
}

// `scsi.hd_models` — V_LIST of {label, vendor, product, size} maps for the
// known SCSI HD model catalog. Each entry is rendered as a JSON-ish string
// for now (V_LIST<V_STRING>) since the value substrate doesn't have a map
// type yet; UI code can JSON.parse each entry.
static value_t scsi_attr_hd_models(struct object *self, const member_t *m) {
    (void)self;
    (void)m;
    int count = drive_catalog_count();
    if (count <= 0)
        return val_list(NULL, 0);
    value_t *items = (value_t *)calloc((size_t)count, sizeof(value_t));
    if (!items)
        return val_err("scsi.hd_models: out of memory");
    for (int i = 0; i < count; i++) {
        const struct drive_model *md = drive_catalog_get(i);
        char buf[192];
        snprintf(buf, sizeof(buf), "{\"label\":\"%s\",\"vendor\":\"%s\",\"product\":\"%s\",\"size\":%zu}",
                 md ? md->label : "", md ? md->vendor : "", md ? md->product : "", md ? md->size : 0);
        items[i] = val_str(buf);
    }
    return val_list(items, (size_t)count);
}

static const arg_decl_t scsi_path_arg[] = {
    {.name = "path", .kind = V_STRING, .doc = "Image file path"},
};

static const arg_decl_t scsi_attach_args[] = {
    {.name = "path", .kind = V_STRING, .doc = "Image file path"    },
    {.name = "id",   .kind = V_INT,    .doc = "SCSI bus index 0..6"},
};

static const member_t scsi_members[] = {
    {.kind = M_ATTR,
     .name = "loopback",
     .doc = "Loopback test card / passive terminator",
     .flags = 0,
     .attr = {.type = V_BOOL, .get = scsi_attr_loopback_get, .set = scsi_attr_loopback_set}},
    {.kind = M_ATTR,
     .name = "hd_models",
     .doc = "Known SCSI HD model catalog (list of JSON-encoded entries)",
     .flags = VAL_RO,
     .attr = {.type = V_LIST, .get = scsi_attr_hd_models, .set = NULL}},
    {.kind = M_METHOD,
     .name = "identify_hd",
     .doc = "True if the file looks like a SCSI HD image",
     .method = {.args = scsi_path_arg, .nargs = 1, .result = V_BOOL, .fn = scsi_method_identify_hd}},
    {.kind = M_METHOD,
     .name = "identify_cdrom",
     .doc = "True if the file looks like a CD-ROM image",
     .method = {.args = scsi_path_arg, .nargs = 1, .result = V_BOOL, .fn = scsi_method_identify_cdrom}},
    {.kind = M_METHOD,
     .name = "attach_hd",
     .doc = "Attach a hard-disk image at the given SCSI id",
     .method = {.args = scsi_attach_args, .nargs = 2, .result = V_BOOL, .fn = scsi_method_attach_hd}},
    {.kind = M_METHOD,
     .name = "attach_cdrom",
     .doc = "Attach a CD-ROM image at the given SCSI id",
     .method = {.args = scsi_attach_args, .nargs = 2, .result = V_BOOL, .fn = scsi_method_attach_cdrom}},
};

const class_desc_t scsi_class = {
    .name = "scsi",
    .members = scsi_members,
    .n_members = sizeof(scsi_members) / sizeof(scsi_members[0]),
};
