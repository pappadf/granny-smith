// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// scsi.c
// Implements SCSI controller emulation for the NCR 5380

#define _CRT_SECURE_NO_WARNINGS 1

#include "scsi.h"
#include "platform.h"
#include "scsi_internal.h"
#include "shell.h"
#include "system.h"

#include <assert.h>
#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Constants and Macros
// ============================================================================

// #define LOG(...) log_message(LOG_SCSI, 1, __VA_ARGS__)
#define LOG(...)
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
    scsi_update_irq(scsi);
}

// Transition SCSI bus to data-out phase (initiator to target)
void phase_data_out(scsi_t *scsi, int bytes) {
    assert(scsi->bus.phase == scsi_command);
    assert(bytes <= BUF_LIMIT);

    scsi->bus.phase = scsi_data_out;
    scsi->reg.csr = CSR_REQ + CSR_BSY;
    scsi->buf.max = bytes;
    scsi->buf.size = 0;
    scsi_update_irq(scsi);
}

// Transition SCSI bus to status phase
void phase_status(scsi_t *scsi, uint8_t status) {
    assert(scsi->bus.phase == scsi_command || scsi->bus.phase == scsi_data_in || scsi->bus.phase == scsi_data_out);

    scsi->bus.phase = scsi_status;
    scsi->reg.csr = CSR_IO + CSR_CD + CSR_REQ + CSR_BSY;
    scsi->reg.cdr = status;
    // Signal end-of-DMA to the IRQ logic if DMA was active
    if (scsi->reg.mr & MR_DMA)
        scsi->end_of_dma = true;
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
        LOG("command: TEST UNIT READY");
        // Check if medium is present for CD-ROM
        if (scsi->devices[target].type == scsi_dev_cdrom && !scsi->devices[target].medium_present) {
            scsi_check_condition(scsi, SENSE_NOT_READY, ASC_MEDIUM_NOT_PRESENT, 0x00);
        } else {
            phase_status(scsi, STATUS_GOOD);
        }
        break;

    case CMD_REZERO_UNIT:
        LOG("command: REZERO UNIT");
        // Seek to block 0 — no-op in emulation
        phase_status(scsi, STATUS_GOOD);
        break;

    case CMD_REQUEST_SENSE:
        scsi_cdrom_request_sense(scsi);
        break;

    case CMD_FORMAT_UNIT:
        LOG("command: FORMAT UNIT");
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
            size_t n = disk_read_data(scsi->devices[target].image, byte_off, scsi->buf.data, byte_cnt);
            assert(n == byte_cnt);
        }
        break;
    }

    case CMD_SEEK_6:
    case CMD_SEEK_10:
        // Seek to LBA — no-op (no seek latency to emulate)
        LOG("command: SEEK");
        phase_status(scsi, STATUS_GOOD);
        break;

    case CMD_INQUIRY:

        assert(scsi->buf.data != NULL);
        assert(scsi->devices[target].image != NULL);

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

    case CMD_MODE_SELECT:
        // Dispatch MODE SELECT to CD-ROM handler if applicable
        if (scsi->devices[target].type == scsi_dev_cdrom) {
            // Accept the data phase, then CD-ROM handler processes it
            int param_len = scsi->buf.data[4];
            if (param_len > 0)
                phase_data_out(scsi, param_len);
            else
                phase_status(scsi, STATUS_GOOD);
        } else {
            phase_data_out(scsi, scsi->buf.data[4]);
        }
        break;

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

        assert((scsi->buf.data[8] & 1) == 0); // PMI = 0

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
        LOG("command: VERIFY");
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

        // let's assume that we have an identified initiator id
        assert(scsi->bus.initiator < 8);

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
        else if (scsi->bus.phase == scsi_data_in) {
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
        scsi_update_drq(scsi);
        scsi_update_irq(scsi);
    } else if (bits_set & MR_DMA) {

        // DMA mode can be set during data_in/data_out (normal) or during
        // status/message_in (if command returned CHECK CONDITION before the
        // driver expected a data phase — the NCR 5380 signals this as a
        // phase mismatch IRQ so the driver can recover).
        assert(scsi->bus.phase == scsi_data_in || scsi->bus.phase == scsi_data_out || scsi->bus.phase == scsi_status ||
               scsi->bus.phase == scsi_message_in);

        // if we're reading in data, and there is more in the buffer - then assert request signal
        if (scsi->bus.phase == scsi_data_in && scsi->buf.size != 0)
            scsi->reg.bsr |= BSR_DR;

        // if we're writing out data, and there is room in the buffer - then assert request signal
        if (scsi->bus.phase == scsi_data_out && scsi->buf.size < scsi->buf.max)
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
                if (scsi->reg.mr & MR_DMA)
                    scsi->reg.csr &= ~CSR_REQ;
            } else
                phase_status(scsi, STATUS_GOOD);
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

            assert(scsi->bus.phase == scsi_data_out);
            assert(scsi->buf.size < scsi->buf.max);

            scsi->buf.data[scsi->buf.size++] = value;

            if (scsi->buf.size == scsi->buf.max)
                command_complete(scsi);
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

// ============================================================================
// Lifecycle: Destructor
// ============================================================================

// Free resources associated with SCSI controller
void scsi_delete(scsi_t *scsi) {
    if (!scsi)
        return;
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
