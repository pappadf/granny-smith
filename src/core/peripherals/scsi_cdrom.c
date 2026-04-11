// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// scsi_cdrom.c
// CD-ROM device logic for the SCSI subsystem (AppleCD SC Plus / Sony CDU-8002).

#include "platform.h"
#include "scsi.h"
#include "scsi_internal.h"

#include <assert.h>
#include <string.h>

// ============================================================================
// MODE SENSE Pages
// ============================================================================

// Build Mode Page 0x01: Read Error Recovery Parameters (8 bytes)
static int build_page_01(uint8_t *buf) {
    buf[0] = 0x01; // page code
    buf[1] = 0x06; // page length
    buf[2] = 0x00; // error recovery: no retries
    buf[3] = 0x01; // read retry count
    buf[4] = 0x00; // reserved
    buf[5] = 0x00; // reserved
    buf[6] = 0x00; // reserved
    buf[7] = 0x00; // reserved
    return 8;
}

// Build Mode Page 0x02: Disconnect-Reconnect Parameters (16 bytes)
static int build_page_02(uint8_t *buf) {
    buf[0] = 0x02; // page code
    buf[1] = 0x0E; // page length
    memset(buf + 2, 0, 14); // all zeros (we don't disconnect)
    return 16;
}

// Build Mode Page 0x08: Caching Parameters (12 bytes)
static int build_page_08(uint8_t *buf) {
    buf[0] = 0x08; // page code
    buf[1] = 0x0A; // page length
    memset(buf + 2, 0, 10);
    return 12;
}

// Build Mode Page 0x09: Audio Control Parameters (16 bytes)
static int build_page_09(uint8_t *buf) {
    buf[0] = 0x09; // page code
    buf[1] = 0x0E; // page length
    memset(buf + 2, 0, 14);
    // Output port 0 channel selection = 01 (left)
    buf[8] = 0x01;
    buf[9] = 0xFF; // volume
    // Output port 1 channel selection = 02 (right)
    buf[10] = 0x02;
    buf[11] = 0xFF; // volume
    return 16;
}

// Build Mode Page 0x30: Apple Vendor Page (32 bytes).
// The real CDU-8002 (1991, SCSI-1) may predate this mechanism, but Apple's
// later drivers (System 7.5+) request page 0x30 even from older drives.
// QEMU enables it unconditionally. We do the same for compatibility.
static int build_page_30(uint8_t *buf, int page_control) {
    buf[0] = 0x30; // page code
    buf[1] = 0x1E; // page length = 30 bytes
    if (page_control == 1) {
        // Changeable values: return all zeros
        memset(buf + 2, 0, 30);
    } else {
        // Current/default values: Apple vendor string
        memcpy(buf + 2, "APPLE COMPUTER, INC   ", 22);
        memset(buf + 24, 0, 8);
    }
    return 32;
}

// ============================================================================
// MODE SENSE(6) Handler
// ============================================================================

// Handle MODE SENSE(6) for CD-ROM device
void scsi_cdrom_mode_sense(scsi_t *scsi) {
    int target = scsi->bus.target & 7;
    uint8_t alloc_len = scsi->buf.data[4];
    uint8_t page_code = scsi->buf.data[2] & 0x3F;
    int page_control = (scsi->buf.data[2] >> 6) & 0x03;

    // Build response in the SCSI buffer
    uint8_t *buf = scsi->buf.data;
    memset(buf, 0, BUF_LIMIT < 256 ? BUF_LIMIT : 256);

    // Mode parameter header (4 bytes)
    int pos = 4;

    // Block descriptor (8 bytes) — always present (A/UX requires it)
    uint16_t blk_sz = scsi->devices[target].block_size;
    uint32_t blocks = 0;
    if (scsi->devices[target].image)
        blocks = (uint32_t)(disk_size(scsi->devices[target].image) / blk_sz);

    buf[3] = 8; // block descriptor length
    buf[pos + 0] = 0; // density code
    buf[pos + 1] = (blocks >> 16) & 0xFF; // number of blocks
    buf[pos + 2] = (blocks >> 8) & 0xFF;
    buf[pos + 3] = blocks & 0xFF;
    buf[pos + 4] = 0; // reserved
    buf[pos + 5] = (blk_sz >> 16) & 0xFF; // block length
    buf[pos + 6] = (blk_sz >> 8) & 0xFF;
    buf[pos + 7] = blk_sz & 0xFF;
    pos += 8;

    // Append requested mode pages
    switch (page_code) {
    case 0x01:
        pos += build_page_01(buf + pos);
        break;
    case 0x02:
        pos += build_page_02(buf + pos);
        break;
    case 0x08:
        pos += build_page_08(buf + pos);
        break;
    case 0x09:
        pos += build_page_09(buf + pos);
        break;
    case 0x30:
        pos += build_page_30(buf + pos, page_control);
        break;
    case 0x3F:
        // Return all pages
        pos += build_page_01(buf + pos);
        pos += build_page_02(buf + pos);
        pos += build_page_08(buf + pos);
        pos += build_page_09(buf + pos);
        pos += build_page_30(buf + pos, page_control);
        break;
    case 0x00:
        // Vendor-specific page 0 — return just the header + block descriptor
        break;
    default:
        // Unknown page — return just header + block descriptor (like real hardware)
        break;
    }

    // Fill in the mode data length (byte 0 = total length - 1)
    buf[0] = (uint8_t)(pos - 1);

    // Clamp to allocation length
    int len = pos;
    if (alloc_len > 0 && len > alloc_len)
        len = alloc_len;

    phase_data_in(scsi, len);
}

// ============================================================================
// MODE SELECT(6) Handler
// ============================================================================

// Handle MODE SELECT(6) data for CD-ROM device (called after data-out phase)
void scsi_cdrom_mode_select(scsi_t *scsi) {
    int target = scsi->bus.target & 7;
    uint8_t *data = scsi->buf.data;
    int len = (int)scsi->buf.size;

    // Parse mode parameter header (4 bytes minimum)
    if (len < 4) {
        phase_status(scsi, STATUS_GOOD);
        return;
    }

    int bd_len = data[3]; // block descriptor length
    int offset = 4; // skip header

    // Parse block descriptor if present — detect 512-byte sector switch (A/UX)
    if (bd_len >= 8 && offset + 8 <= len) {
        uint32_t block_len =
            ((uint32_t)data[offset + 5] << 16) | ((uint32_t)data[offset + 6] << 8) | (uint32_t)data[offset + 7];
        // Switch block size if the host requests 512 or 2048
        if (block_len == 512 || block_len == 2048)
            scsi->devices[target].block_size = (uint16_t)block_len;
    }

    // Accept any remaining page data silently (truncated MODE SELECT is OK).
    // PF=0 (byte 1 bit 4 cleared) for page 0x00 is accepted per A/UX compat.
    phase_status(scsi, STATUS_GOOD);
}

// ============================================================================
// REQUEST SENSE Handler
// ============================================================================

// Handle REQUEST SENSE command (shared between HD and CD-ROM)
void scsi_cdrom_request_sense(scsi_t *scsi) {
    int target = scsi->bus.target & 7;
    int alloc_len = scsi->buf.data[4];
    if (alloc_len == 0)
        alloc_len = 18; // default sense data length

    int len = alloc_len < 18 ? alloc_len : 18;
    phase_data_in(scsi, len);

    memset(scsi->buf.data, 0, len);

    // Extended sense data format (error code 0x70)
    scsi->buf.data[0] = 0x70; // current errors, fixed format
    scsi->buf.data[2] = scsi->devices[target].sense.key;
    scsi->buf.data[7] = 0x0A; // additional sense length
    if (len > 12)
        scsi->buf.data[12] = scsi->devices[target].sense.asc;
    if (len > 13)
        scsi->buf.data[13] = scsi->devices[target].sense.ascq;

    // Clear sense data after reporting
    memset(&scsi->devices[target].sense, 0, sizeof(scsi->devices[target].sense));

    // Clear pending UNIT ATTENTION after reporting via REQUEST SENSE
    scsi->devices[target].unit_attention = false;
}

// ============================================================================
// READ TOC Handler
// ============================================================================

// Handle READ TOC command — return minimal single-track data TOC
void scsi_cdrom_read_toc(scsi_t *scsi) {
    int target = scsi->bus.target & 7;
    uint16_t alloc_len = (scsi->buf.data[7] << 8) | scsi->buf.data[8];
    // uint8_t format = scsi->buf.data[2] & 0x0F; // format code (unused for now)

    // For a single data session, return: header + track 1 + lead-out
    uint8_t toc[20];
    memset(toc, 0, sizeof(toc));

    // TOC header (4 bytes)
    toc[0] = 0x00; // data length MSB
    toc[1] = 0x12; // data length LSB (18 = 2 track descriptors * 8 + 2)
    toc[2] = 0x01; // first track
    toc[3] = 0x01; // last track

    // Track 1 descriptor (8 bytes): data track at LBA 0
    toc[4] = 0x00; // reserved
    toc[5] = 0x14; // ADR=1, control=4 (data track, no copy permission)
    toc[6] = 0x01; // track number
    toc[7] = 0x00; // reserved
    // LBA = 0 (bytes 8-11)

    // Lead-out descriptor (8 bytes): track 0xAA at total blocks
    toc[12] = 0x00; // reserved
    toc[13] = 0x14; // ADR=1, control=4
    toc[14] = 0xAA; // lead-out track
    toc[15] = 0x00; // reserved
    // LBA = total blocks
    uint16_t blk_sz = scsi->devices[target].block_size;
    uint32_t total = 0;
    if (scsi->devices[target].image)
        total = (uint32_t)(disk_size(scsi->devices[target].image) / blk_sz);
    toc[16] = (total >> 24) & 0xFF;
    toc[17] = (total >> 16) & 0xFF;
    toc[18] = (total >> 8) & 0xFF;
    toc[19] = total & 0xFF;

    int len = 20;
    if (alloc_len > 0 && len > alloc_len)
        len = alloc_len;

    phase_data_in(scsi, len);
    memcpy(scsi->buf.data, toc, len);
}

// ============================================================================
// READ SUB-CHANNEL Handler
// ============================================================================

// Handle READ SUB-CHANNEL — return "no current audio status" for data disc
void scsi_cdrom_read_sub_channel(scsi_t *scsi) {
    uint16_t alloc_len = (scsi->buf.data[7] << 8) | scsi->buf.data[8];

    // Minimal sub-channel data header (4 bytes)
    uint8_t resp[4];
    memset(resp, 0, sizeof(resp));
    resp[1] = 0x15; // audio status: no current audio status info

    int len = 4;
    if (alloc_len > 0 && len > alloc_len)
        len = alloc_len;

    phase_data_in(scsi, len);
    memcpy(scsi->buf.data, resp, len);
}

// ============================================================================
// READ HEADER Handler
// ============================================================================

// Handle READ HEADER — return mode 1 data for the requested LBA
void scsi_cdrom_read_header(scsi_t *scsi) {
    uint16_t alloc_len = (scsi->buf.data[7] << 8) | scsi->buf.data[8];
    uint32_t lba = (scsi->buf.data[2] << 24) | (scsi->buf.data[3] << 16) | (scsi->buf.data[4] << 8) | scsi->buf.data[5];

    uint8_t resp[8];
    memset(resp, 0, sizeof(resp));
    resp[0] = 0x01; // CD-ROM data mode 1
    // Bytes 4-7: absolute block address
    resp[4] = (lba >> 24) & 0xFF;
    resp[5] = (lba >> 16) & 0xFF;
    resp[6] = (lba >> 8) & 0xFF;
    resp[7] = lba & 0xFF;

    int len = 8;
    if (alloc_len > 0 && len > alloc_len)
        len = alloc_len;

    phase_data_in(scsi, len);
    memcpy(scsi->buf.data, resp, len);
}

// ============================================================================
// START/STOP UNIT Handler
// ============================================================================

// Handle START/STOP UNIT command for CD-ROM
void scsi_cdrom_start_stop_unit(scsi_t *scsi) {
    int target = scsi->bus.target & 7;
    uint8_t flags = scsi->buf.data[4];
    bool start = (flags & 0x01) != 0;
    bool loej = (flags & 0x02) != 0;

    if (!start && loej) {
        // Eject: check if removal is prevented
        if (scsi->devices[target].prevent_removal) {
            scsi_check_condition(scsi, SENSE_ILLEGAL_REQUEST, ASC_MEDIUM_NOT_PRESENT, 0x00);
            return;
        }
        // Mark medium as not present (eject)
        scsi->devices[target].medium_present = false;
        scsi->devices[target].image = NULL;
        scsi->devices[target].unit_attention = true;
        scsi_set_sense(scsi, target, SENSE_UNIT_ATTENTION, ASC_MEDIUM_NOT_PRESENT, 0x00);
    }
    // Start=1 (spin up) or Start=0,LoEj=0 (spin down): no-op
    phase_status(scsi, STATUS_GOOD);
}

// ============================================================================
// PREVENT/ALLOW MEDIUM REMOVAL Handler
// ============================================================================

// Handle PREVENT/ALLOW MEDIUM REMOVAL command for CD-ROM
void scsi_cdrom_prevent_allow(scsi_t *scsi) {
    int target = scsi->bus.target & 7;
    scsi->devices[target].prevent_removal = (scsi->buf.data[4] & 0x01) != 0;
    phase_status(scsi, STATUS_GOOD);
}
