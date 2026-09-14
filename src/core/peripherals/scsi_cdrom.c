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
//
// CDU-541 manual S5.2.3.2, changeable values: "The page requested will be
// returned with the bits that are allowed to be changed set to one.
// Parameters that are not changeable will be set to zero. ... The page
// descriptor ... will always be returned even if none of parameters are
// changeable within the page."  Nothing here is changeable, so PC=1 keeps the
// header and zeroes the body.
static int build_page_01(uint8_t *buf, int page_control) {
    buf[0] = 0x01; // page code
    buf[1] = 0x06; // page length
    memset(buf + 2, 0, 6);
    if (page_control == 1)
        return 8;
    buf[2] = 0x00; // error recovery parameter
    // CDU-541 manual S5.3.1.1: "The read retry count field specifies the number
    // of times that the controller will attempt its read recovery algorithm.
    // The default value is ZERO."  This emitted 1, and scsi_cdrom.md S4.3 said
    // 3; neither is the drive's.
    buf[3] = 0x00; // read retry count
    buf[4] = 0x00; // reserved
    buf[5] = 0x00; // reserved
    buf[6] = 0x00; // reserved
    buf[7] = 0x00; // reserved
    return 8;
}

// Build Mode Page 0x07: Verify Error Recovery Parameters (8 bytes)
//
// Table 5-47 lists this drive's MODE SENSE pages as 01h, 02h, 07h, 08h, 09h and
// 3Fh; this one was missing, so a host asking for it -- or for all pages -- got
// a GOOD status and no page.
//
// S5.3.1.3 gives it no table of its own that this OCR can read (Table 5-41 is a
// scanned image), only the sentence that defines it: "The implementation of
// error recovery procedures for verification operations is the same as for read
// operations on CD-ROM devices."  So it is page 01's structure with page 01's
// defaults and a different page code -- INFERRED FROM THAT PROSE, not read off
// the table.  SCSI-2 S9.3.3.8 gives page 07h a parameter length of 0Ah rather
// than the 06h used here; this drive declares SCSI-1 in INQUIRY and predates
// that standard by four years, so the manual wins.
static int build_page_07(uint8_t *buf, int page_control) {
    buf[0] = 0x07; // page code
    buf[1] = 0x06; // page length
    memset(buf + 2, 0, 6);
    if (page_control == 1)
        return 8; // nothing changeable: header, zero body
    buf[2] = 0x00; // error recovery parameter
    buf[3] = 0x00; // verify retry count -- "the same as for read operations"
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
static int build_page_09(uint8_t *buf, int page_control) {
    buf[0] = 0x09; // page code
    buf[1] = 0x0E; // page length
    memset(buf + 2, 0, 14);
    if (page_control == 1)
        return 16; // nothing here is changeable: header, zero body
    // Output port 0 channel selection = 01 (left)
    buf[8] = 0x01;
    buf[9] = 0xFF; // volume
    // Output port 1 channel selection = 02 (right)
    buf[10] = 0x02;
    buf[11] = 0xFF; // volume
    return 16;
}

// Mode page $30, Apple's vendor identification, in the CD-ROM's form.
//
// The emitter is shared with the hard-disk path (scsi_build_apple_page_30);
// the STRING is not, and deliberately so -- see the comment there for the
// evidence behind each.  In short: the HD form is verified against HD SC
// Setup, this one is not verified by anything.  No test in this tree requests
// it; instrumenting this function across se30-cdrom and iici-cdrom-boot counts
// zero calls, which fits the CDU-8002 being a 1991 SCSI-1 drive while page $30
// arrived with System 7.5+ drivers.
//
// 22 bytes of string inside a 30-byte page, so the remaining 8 are zero.  The
// string carries no trailing period, unlike the hard disk's.
static int build_page_30(uint8_t *buf, int page_control) {
    static const char apple_cd_id[] = "APPLE COMPUTER, INC   ";
    return scsi_build_apple_page_30(buf, page_control, apple_cd_id, (int)sizeof(apple_cd_id) - 1, 30);
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

    // Build response in the SCSI buffer. The mode-sense response can grow
    // past 256 bytes for page_code == 0x3F (all pages), so zero the *whole*
    // BUF_LIMIT region rather than just the first 256 bytes — otherwise stale
    // payload from a previous command leaks into the unzeroed tail.
    uint8_t *buf = scsi->buf.data;
    memset(buf, 0, BUF_LIMIT);

    // Mode parameter header (4 bytes)
    int pos = 4;

    // Block descriptor (8 bytes) — always present (A/UX requires it).
    //
    // Its block length answers to the page control field, which the rest of
    // this command used to ignore.  CDU-541 manual S5.2.3: "The default block
    // length is 2048 and is returned if default values are requested.  The
    // current block length is returned if current values are requested.  A
    // block length of FFh FFh FFh is returned if changeable values are
    // requested."  Block length IS changeable on this drive -- Table 5-4 lists
    // 256, 512, 1024, 2048 and 2336 -- so the changeable answer sets every bit
    // of the field, not zero.
    //
    // The default matters in practice: A/UX switches the disc to 512-byte
    // blocks (see the MODE SELECT path below), after which a PC=2 request must
    // still answer 2048.
    uint16_t blk_sz = scsi->devices[target].block_size;
    uint32_t reported_blk_sz = blk_sz;
    if (page_control == 1)
        reported_blk_sz = 0xFFFFFFu; // every bit of a changeable field
    else if (page_control == 2 || page_control == 3)
        reported_blk_sz = scsi->devices[target].default_block_size;
    uint32_t blocks = 0;
    if (scsi->device_images[target])
        blocks = (uint32_t)(disk_size(scsi->device_images[target]) / blk_sz);

    buf[3] = 8; // block descriptor length
    buf[pos + 0] = 0; // density code
    buf[pos + 1] = (blocks >> 16) & 0xFF; // number of blocks
    buf[pos + 2] = (blocks >> 8) & 0xFF;
    buf[pos + 3] = blocks & 0xFF;
    buf[pos + 4] = 0; // reserved
    buf[pos + 5] = (reported_blk_sz >> 16) & 0xFF; // block length, per PC above
    buf[pos + 6] = (reported_blk_sz >> 8) & 0xFF;
    buf[pos + 7] = reported_blk_sz & 0xFF;
    pos += 8;

    // Append requested mode pages
    switch (page_code) {
    case 0x01:
        pos += build_page_01(buf + pos, page_control);
        break;
    case 0x02:
        pos += build_page_02(buf + pos);
        break;
    case 0x07:
        pos += build_page_07(buf + pos, page_control);
        break;
    case 0x08:
        pos += build_page_08(buf + pos);
        break;
    case 0x09:
        pos += build_page_09(buf + pos, page_control);
        break;
    case 0x30:
        pos += build_page_30(buf + pos, page_control);
        break;
    case 0x3F:
        // Return all pages
        pos += build_page_01(buf + pos, page_control);
        pos += build_page_02(buf + pos);
        pos += build_page_07(buf + pos, page_control);
        pos += build_page_08(buf + pos);
        pos += build_page_09(buf + pos, page_control);
        pos += build_page_30(buf + pos, page_control);
        break;
    case 0x00:
        // Vendor-specific page 0 — return just the header + block descriptor
        break;
    default:
        // CDU-541 manual S5.2.3: "If the page code specified is not implemented
        // the command will be terminated with a CHECK CONDITION status.  The
        // sense key will be set to ILLEGAL REQUEST and the additional sense code
        // set to ILLEGAL VALUE IN CDB."
        //
        // This used to return header-plus-block-descriptor with GOOD status and
        // a comment claiming that was "like real hardware".  It is the opposite:
        // a host that asks for a page it does not get, and is told everything
        // went well, parses whatever its own buffer held -- the failure the hard
        // disk path's page 3/4 comment documents, where MkLinux DR3's rz driver
        // derived a 14384-byte sector from the leftovers of its own INQUIRY.
        scsi_check_condition(scsi, SENSE_ILLEGAL_REQUEST, ASC_INVALID_FIELD_IN_CDB, 0x00);
        return;
    }

    // Fill in the mode data length (byte 0 = total length - 1). The field is
    // a single byte, so clamp to avoid silent truncation if the assembled
    // response ever grew past 256 bytes.
    int data_len = pos - 1;
    if (data_len > 255)
        data_len = 255;
    buf[0] = (uint8_t)data_len;

    // Bound by the allocation length.  Zero means zero -- CDU-541 manual
    // S4.2.6 -- where this used to read `alloc_len > 0 &&`, i.e. send the whole
    // response to a probe that allocated nothing for it.
    scsi_data_in_alloc(scsi, pos, alloc_len);
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

    // Zero means zero here too.  ANSI X3.131-1986's REQUEST SENSE section is
    // the one place a zero allocation names a non-zero answer -- "four bytes of
    // sense data shall be transferred" -- but those four bytes are the
    // NONEXTENDED sense format (Table 7-4), which this model does not
    // implement: S7.1.2's implementors note frames the rule as how a target
    // supporting BOTH formats selects between them.  Four bytes of our extended
    // ($70) block would be a truncated header, not that format.  The CDU-541
    // manual S4.2.6, which governs the drive we advertise, has no exception at
    // all.  This used to substitute 18.
    int len = scsi_data_in_alloc(scsi, 18, alloc_len);
    if (len == 0)
        return;

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
// A CD address in MSF form.  X3.131-1994 S14.1.5 and Table 237: the four-byte
// address field becomes reserved / M / S / F when the CDB's MSF bit is set.
//
// The +150 is the Red Book two-second lead-in: LBA 0 is at 00:02:00, and a
// frame is 1/75 s, so 2 * 75 = 150 frames separate the two origins.  X3.131
// S14.1.5 states the ratios are the drive's to report ("The ratios of M field
// units to S field units and S field units to F field units are reported in the
// mode parameters page"), and 60/75 is what a CD is.
static void lba_to_msf(uint32_t lba, uint8_t out[4]) {
    uint32_t f = lba + 150u;
    out[0] = 0x00; // reserved
    out[1] = (uint8_t)(f / (60u * 75u)); // M
    out[2] = (uint8_t)((f / 75u) % 60u); // S
    out[3] = (uint8_t)(f % 75u); // F
}

// Write a CD address into a four-byte field, as an LBA or as MSF.
static void put_cd_address(uint8_t *dst, uint32_t lba, bool msf) {
    if (msf) {
        lba_to_msf(lba, dst);
        return;
    }
    dst[0] = (uint8_t)(lba >> 24);
    dst[1] = (uint8_t)(lba >> 16);
    dst[2] = (uint8_t)(lba >> 8);
    dst[3] = (uint8_t)lba;
}

void scsi_cdrom_read_toc(scsi_t *scsi) {
    int target = scsi->bus.target & 7;
    uint16_t alloc_len = (scsi->buf.data[7] << 8) | scsi->buf.data[8];
    // MSF: X3.131-1994 Table 260 puts it at byte 1 bit 1, and CDU-541 S5.2.24
    // agrees -- "the format of the CD Address is determined by the MSF bit in
    // the CDB".  Byte 2 is Reserved in BOTH; there is no format field here.
    // (A commented-out `format = data[2] & 0x0F` used to sit on this line.
    // Format codes and Format 1 session info are MMC, a later standard than
    // either authority for this drive, which reports ANSI version 0x01.)
    bool msf = (scsi->buf.data[1] & 0x02) != 0;

    // Starting track (byte 6).  A single-session data disc has exactly track 1,
    // so only 1h and AAh (lead-out) can be satisfied; anything else names a
    // track this disc does not have.  Both authorities agree on the answer for
    // that -- CDU-541 S5.2.24: "If the track number field is zero or is not
    // valid for the disc inserted the command will be terminated with a CHECK
    // CONDITION status.  The sense key is set to ILLEGAL REQUEST.  The
    // additional sense code is set to ILLEGAL VALUE IN CDB."  X3.131-1994
    // S14.2.11 says the same with INVALID FIELD IN CDB.
    //
    // They disagree about ZERO, and this takes the lenient reading:
    //
    //   CDU-541:      zero is invalid, listed alongside out-of-range.
    //   X3.131-1994:  "If this value is zero, the table of contents data shall
    //                  begin with the first track on the medium."
    //
    // Zero is accepted as "from the first track", for two reasons.  The Sony
    // C1h handler below already does exactly that, deliberately, and one drive
    // answering the same question two ways would be worse than either answer.
    // And refusing a value a real driver may legitimately send, on a path with
    // no test coverage at all -- measured zero calls across se30-cdrom,
    // iici-cdrom-boot and iicx-mactest -- is the more expensive way to be
    // wrong.
    uint8_t start_track = scsi->buf.data[6];
    if (start_track > 0x01 && start_track != 0xAA) {
        scsi_check_condition(scsi, SENSE_ILLEGAL_REQUEST, ASC_INVALID_FIELD_IN_CDB, 0x00);
        return;
    }

    uint16_t blk_sz = scsi->devices[target].block_size;
    uint32_t total = 0;
    if (scsi->device_images[target])
        total = (uint32_t)(disk_size(scsi->device_images[target]) / blk_sz);

    // A single data session: track 1 at LBA 0, then the lead-out.  AAh asks for
    // the lead-out alone, so the descriptor list is one entry shorter.
    uint8_t toc[20];
    memset(toc, 0, sizeof(toc));
    int pos = 4;

    if (start_track <= 0x01) { // 0 means "from the first track"
        toc[pos + 0] = 0x00; // reserved
        toc[pos + 1] = 0x14; // ADR=1, control=4 (data track, no copy permission)
        toc[pos + 2] = 0x01; // track number
        toc[pos + 3] = 0x00; // reserved
        put_cd_address(&toc[pos + 4], 0, msf);
        pos += 8;
    }
    toc[pos + 0] = 0x00; // reserved
    toc[pos + 1] = 0x14; // ADR=1, control=4
    toc[pos + 2] = 0xAA; // lead-out track
    toc[pos + 3] = 0x00; // reserved
    put_cd_address(&toc[pos + 4], total, msf);
    pos += 8;

    // TOC header.  The data length is the length AVAILABLE for this request --
    // X3.131-1994 Table 261: "the length in bytes of the following TOC data that
    // is available to be transferred", CDU-541 S5.2.24: "the length in bytes of
    // the available table of contents data.  The value of TOC data length does
    // not include itself."
    //
    // So it does NOT shrink when the allocation length truncates the transfer;
    // that is how the initiator learns there is more to ask for.  It does vary
    // with the REQUEST: AAh yields one descriptor, 01h yields two.
    toc[0] = 0x00;
    toc[1] = (uint8_t)(pos - 2);
    toc[2] = 0x01; // first track
    toc[3] = 0x01; // last track

    int len = scsi_data_in_alloc(scsi, pos, alloc_len);
    if (len > 0)
        memcpy(scsi->buf.data, toc, (size_t)len);
}

// Handle the Sony vendor READ TOC (C1h) — returns the CDU-541 "TOC Data Format"
// (Sony CDU-541 SCSI Interface Manual §5.2.23, Table 5-23), which is NOT the
// SCSI-2 (43h) layout: a 4-byte header followed by *6-byte* track descriptors
//   [track#] [reserved(hi nibble) | control(lo nibble)] [4-byte CD address]
// rather than the 8-byte SCSI-2 descriptor [rsvd][ADR|control][track#][rsvd][4].
// The control low nibble is defined in Table 5-24: bit 2 = 1 -> data track,
// 0 -> audio track.  Early Apple CD-ROM drivers (System 7.x) drive the AppleCD
// SC's Sony CDU-8002 with this vendor command exclusively; if we answer C1h with
// the 43h byte layout, the driver reads the control nibble from the wrong offset
// (it lands on an SCSI-2 padding byte = 0) and mis-classifies the data disc as an
// audio CD.  The CD address is returned as an LBA (the LBAMSF bit default in the
// CD-ROM parameters page; we do not model MSF addressing for the TOC).
void scsi_cdrom_read_toc_sony(scsi_t *scsi) {
    int target = scsi->bus.target & 7;
    // CDB (§5.2.23): byte 5 = starting track, bytes 7-8 = allocation length.
    uint8_t start_track = scsi->buf.data[5];
    uint16_t alloc_len = (uint16_t)((scsi->buf.data[7] << 8) | scsi->buf.data[8]);

    // Single data session: one data track (#1) plus the lead-out (#AAh).
    // The lead-out CD address is the disc's total block count.
    uint16_t blk_sz = scsi->devices[target].block_size;
    uint32_t total = 0;
    if (scsi->device_images[target])
        total = (uint32_t)(disk_size(scsi->device_images[target]) / blk_sz);

    // Include track 1 only when the requested starting track covers it; AAh (or
    // any value past our single track) asks for just the lead-out.  Track 0 is
    // treated leniently as "from the first track" — the real AppleCD firmware
    // does the same even though the generic spec calls 0 an illegal value.
    bool want_track1 = (start_track <= 1);

    uint8_t toc[4 + 6 * 2];
    memset(toc, 0, sizeof(toc));

    // 6-byte track descriptor: [track#][reserved|control][CD address b0..b3].
    // Control = 0x04 (bit 2 set) = data track; high nibble reserved (Table 5-23).
    int pos = 4; // after the 4-byte header
    if (want_track1) {
        toc[pos + 0] = 0x01; // track number 1
        toc[pos + 1] = 0x04; // control: data track
        // CD address = LBA 0 (bytes pos+2..pos+5 already zero)
        pos += 6;
    }
    // Lead-out descriptor (always last).
    toc[pos + 0] = 0xAA; // lead-out track number
    toc[pos + 1] = 0x04; // control: data track
    toc[pos + 2] = (total >> 24) & 0xFF; // CD address = total blocks (LBA)
    toc[pos + 3] = (total >> 16) & 0xFF;
    toc[pos + 4] = (total >> 8) & 0xFF;
    toc[pos + 5] = total & 0xFF;
    pos += 6;

    // 4-byte header: TOC data length (excludes itself) then first/last track.
    uint16_t toc_data_len = (uint16_t)(pos - 2); // first+last (2) + descriptors
    toc[0] = (toc_data_len >> 8) & 0xFF;
    toc[1] = toc_data_len & 0xFF;
    toc[2] = 0x01; // first track number
    toc[3] = 0x01; // last track number

    int len = scsi_data_in_alloc(scsi, pos, alloc_len);
    if (len > 0)
        memcpy(scsi->buf.data, toc, (size_t)len);
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

    int len = scsi_data_in_alloc(scsi, 4, alloc_len);
    if (len > 0)
        memcpy(scsi->buf.data, resp, (size_t)len);
}

// ============================================================================
// READ HEADER Handler
// ============================================================================

// Handle READ HEADER — return mode 1 data for the requested LBA
void scsi_cdrom_read_header(scsi_t *scsi) {
    uint16_t alloc_len = (uint16_t)(((uint16_t)scsi->buf.data[7] << 8) | scsi->buf.data[8]);
    // X3.131-1994 S14.1.5: "The READ HEADER, READ SUB-CHANNEL and READ TABLE OF
    // CONTENTS commands have this feature" -- the MSF bit, byte 1 bit 1.
    bool msf = (scsi->buf.data[1] & 0x02) != 0;
    // Promote each byte to uint32_t before shifting so the high-byte shift
    // (`<< 24`) doesn't trip signed-overflow UB when data[2] > 0x7F.
    uint32_t lba = ((uint32_t)scsi->buf.data[2] << 24) | ((uint32_t)scsi->buf.data[3] << 16) |
                   ((uint32_t)scsi->buf.data[4] << 8) | (uint32_t)scsi->buf.data[5];

    uint8_t resp[8];
    memset(resp, 0, sizeof(resp));
    resp[0] = 0x01; // CD-ROM data mode 1
    put_cd_address(&resp[4], lba, msf); // bytes 4-7: absolute address

    int len = scsi_data_in_alloc(scsi, 8, alloc_len);
    if (len > 0)
        memcpy(scsi->buf.data, resp, (size_t)len);
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
        // Whether the medium may leave the drive is not this command's
        // decision -- the same lock stops the eject button -- so ask the one
        // function that owns it and translate the refusal into SCSI.
        //
        // CDU-541 manual S5.2.33: "a request to eject the disc will be
        // terminated with a CHECK CONDITION status.  The sense key will be set
        // to ILLEGAL REQUEST, and the additional sense code set to PREVENT BIT
        // SET".  This used to report 0x3A MEDIUM NOT PRESENT, which tells the
        // driver the drive is empty -- the opposite of the truth, and a reason
        // to stop retrying.
        if (scsi_eject_device(scsi, target) == -2) {
            scsi_check_condition(scsi, SENSE_ILLEGAL_REQUEST, ASC_SONY_PREVENT_BIT_SET, 0x00);
            return;
        }
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
    bool prevent = (scsi->buf.data[4] & 0x01) != 0;

    // CDU-541 manual S5.2.14: "If a PREVENT MEDIUM REMOVAL command is issued
    // without the drive being in the ready condition [the] command will be
    // terminated with a CHECK CONDITION status.  The sense key will be set to
    // NOT READY and the appropriate additional sense code will be set."  The
    // ready condition is a caddy inserted with its TOC recovered (S4.1.4), so
    // the appropriate code for an empty bay is 0xB0.
    //
    // ALLOW is not covered by that sentence and is not refused: unlocking a
    // drive that has nothing in it is harmless, and a driver tidying up after
    // an eject has every reason to send it.
    if (prevent && !scsi->devices[target].medium_present) {
        scsi_check_condition(scsi, SENSE_NOT_READY, ASC_SONY_CADDY_NOT_INSERTED, 0x00);
        return;
    }

    scsi->devices[target].prevent_removal = prevent;
    phase_status(scsi, STATUS_GOOD);
}
