// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// scsi_cdrom.c
// CD-ROM device logic for the SCSI subsystem: Apple's SCSI CD-ROM drives, from
// the AppleCD SC Plus (Sony CDU-8002) to the AppleCD 24X (Matsushita CR-508).

#include "gs_assert.h"
#include "platform.h"
#include "scsi.h"
#include "scsi_internal.h"

#include <assert.h>
#include <string.h>

// ============================================================================
// Drive families and models
// ============================================================================
//
// Every Apple CD-ROM drive ran Apple firmware on an OEM mechanism, and reports
// an Apple product name rather than the OEM's (a CDU-75S answers "CD-ROM
// CDU-8005", a CR-504 "CD-ROM CR-8005A").  The INQUIRY strings and header bytes
// below are read from the drives' Apple firmware ROMs, where the standard
// INQUIRY response sits as a literal 36-byte block.
//
// The INQUIRY header sorts the drives into firmware families, and the family
// is what decides how a drive answers.  The command handlers in this file and
// in scsi_bus.c are shared by every family; a family only supplies the answers
// that differ (scsi_cd_family_t).

// SONY SCSI-1 caddy drives: the AppleCD SC and SC Plus.  The behaviour every
// drive in this emulator had before models existed, and documented throughout
// this file from the Sony CDU-541 SCSI Interface Manual (1990).  The firmware
// ROM's header reads 05 80 01 01; byte 7 is zero.  The ROM's additional length
// is 31h (a 54-byte response), but this model keeps the 36-byte response it
// has always given.
static const scsi_cd_family_t cd_family_sony_scsi1 = {
    .id = "sony-scsi1",
    .inquiry_version = 0x01, // SCSI-1
    .inquiry_format = 0x01, // CCS
    .inquiry_flags = 0x00,
    .no_medium_asc = ASC_SONY_CADDY_NOT_INSERTED, // B0h, CDU-541 NOT READY table
    .removal_prevented_asc = ASC_SONY_PREVENT_BIT_SET, // 80h, CDU-541 S5.2.33
};

// SONY SCSI-2 caddy drives: the AppleCD 300 and 300i (CDU-8003A).  The ROM's
// header reads 05 80 02 01 1F 00 00 18 -- SCSI-2, but still the CCS response
// format, and Sync and Linked set in byte 7.  Still a caddy drive with Sony
// firmware, so it keeps the Sony sense vocabulary.  NOT VERIFIED: no manual or
// capture of this drive's sense codes is known; B0h is the CDU-541 family's
// code for the same caddy-out condition.
static const scsi_cd_family_t cd_family_sony_scsi2_caddy = {
    .id = "sony-scsi2-caddy",
    .inquiry_version = 0x02, // SCSI-2
    .inquiry_format = 0x01, // CCS
    .inquiry_flags = 0x18, // Sync, Linked
    .no_medium_asc = ASC_SONY_CADDY_NOT_INSERTED,
    .removal_prevented_asc = ASC_SONY_PREVENT_BIT_SET,
};

// SCSI-2 tray drives with Apple firmware, 1994-1997: the Matsushita CR-8004A
// (300i Plus), CR-8005 and CR-8005A (600i), CR-8008 (1200i), CR-8012 (1600i)
// and CR-8024 (24X), and the Sony CDU-8005 (600i).  Every one of those ROMs
// reads 05 80 02 02 1F 00 00 18: SCSI-2, SCSI-2 response format, Sync and
// Linked.  The Sony and the Matsushita 600i present the same header, so they
// are one family here.
//
// A tray drive has no caddy to report, and SCSI-2 has a standard code for an
// empty drive: X3.131-1994 table 74, "Preferred TEST UNIT READY responses",
// NOT READY with 3Ah MEDIUM NOT PRESENT, defined for CD-ROM devices.  NOT
// VERIFIED against a real drive: no manual for these mechanisms is known, and
// no sense capture of an empty one has been published.  It is the answer a
// SCSI-2 driver is written for; a driver that knows only the standard codes
// treats B0h as "becoming ready" and polls.
static const scsi_cd_family_t cd_family_apple_scsi2_tray = {
    .id = "apple-scsi2-tray",
    .inquiry_version = 0x02, // SCSI-2
    .inquiry_format = 0x02, // SCSI-2
    .inquiry_flags = 0x18, // Sync, Linked
    .no_medium_asc = ASC_MEDIUM_NOT_PRESENT, // 3Ah, X3.131-1994 table 74
    .removal_prevented_asc = ASC_MEDIUM_LOAD_EJECT_FAILED, // 53h/02h, X3.131-1994 table 71
    .removal_prevented_ascq = ASCQ_MEDIUM_REMOVAL_PREVENTED,
};

// AppleCD SC Plus and AppleCD 150: Sony CDU-541 mechanism, firmware 1.8g.
// The drive of the Macintosh Plus through the Quadras.
const scsi_cd_drive_t scsi_cd_drive_applecd_sc_plus = {
    .id = "applecd",
    .name = "AppleCD SC Plus",
    .vendor = "SONY",
    .product = "CD-ROM CDU-8002",
    .revision = "1.8g",
    .block_size = 2048,
    .family = &cd_family_sony_scsi1,
};

// AppleCD 300i: Sony CDU-561 mechanism, double speed, caddy; the internal
// drive of the Quadra 660AV and 840AV.
const scsi_cd_drive_t scsi_cd_drive_applecd_300i = {
    .id = "applecd300i",
    .name = "AppleCD 300i",
    .vendor = "SONY",
    .product = "CD-ROM CDU-8003A",
    .revision = "1.9a",
    .block_size = 2048,
    .family = &cd_family_sony_scsi2_caddy,
};

// AppleCD 300i Plus: Matsushita CR-503-K, double speed, tray; the internal
// drive of the Power Macintosh 6100, 7100 and 8100.
const scsi_cd_drive_t scsi_cd_drive_applecd_300i_plus = {
    .id = "applecd300iplus",
    .name = "AppleCD 300i Plus",
    .vendor = "MATSHITA",
    .product = "CD-ROM CR-8004A",
    .revision = "2.0a",
    .block_size = 2048,
    .family = &cd_family_apple_scsi2_tray,
};

// AppleCD 600i: quadruple speed, tray (Apple, Developer Note: Power Macintosh
// 7500 and 8500 Computers, 1995, ch. 3, Table 3-7).  Apple shipped it on a
// Matsushita CR-504 or a Sony CDU-75S under the same part number, 678-0065.
// This is the Matsushita, firmware 4.0i (the CR-504-K).  The internal drive of
// the Power Macintosh 7500, 8500 and 9500 and the Network Server 500 and 700.
const scsi_cd_drive_t scsi_cd_drive_applecd_600i = {
    .id = "applecd600i",
    .name = "AppleCD 600i",
    .vendor = "MATSHITA",
    .product = "CD-ROM CR-8005A",
    .revision = "4.0i",
    .block_size = 2048,
    .family = &cd_family_apple_scsi2_tray,
};

// AppleCD 600i on its Sony mechanism, the CDU-75S-25, Apple firmware 1.0j.
const scsi_cd_drive_t scsi_cd_drive_applecd_600i_sony = {
    .id = "applecd600i-sony",
    .name = "AppleCD 600i (Sony)",
    .vendor = "SONY",
    .product = "CD-ROM CDU-8005",
    .revision = "1.0j",
    .block_size = 2048,
    .family = &cd_family_apple_scsi2_tray,
};

// AppleCD 1200i: Matsushita CR-506-C, eight-times speed, Apple part 678-0090,
// firmware 8.0h.  The 1996 revisions of the Power Macintosh 9500 and 7600.
const scsi_cd_drive_t scsi_cd_drive_applecd_1200i = {
    .id = "applecd1200i",
    .name = "AppleCD 1200i",
    .vendor = "MATSHITA",
    .product = "CD-ROM CR-8008",
    .revision = "8.0h",
    .block_size = 2048,
    .family = &cd_family_apple_scsi2_tray,
};

// AppleCD 1600i, labelled "AppleCD 12X Speed CD-ROM": Matsushita CR-507-C,
// Apple part 678-0109, firmware 1.0g.  Apple, Developer Note: Power Macintosh
// 7300, 7600, 8600, and 9600 Computers (1996): "All the new models include a
// built-in 12X-speed CD-ROM drive, an AppleCD 1600i."
const scsi_cd_drive_t scsi_cd_drive_applecd_1600i = {
    .id = "applecd1600i",
    .name = "AppleCD 1600i",
    .vendor = "MATSHITA",
    .product = "CD-ROM CR-8012",
    .revision = "1.0g",
    .block_size = 2048,
    .family = &cd_family_apple_scsi2_tray,
};

// AppleCD 24X: Matsushita CR-508-C, Apple part 678-0132.  Its Apple firmware
// has not been read; the identity is the one an owner's System Profiler report
// gives, and the family is assumed from its predecessors.
const scsi_cd_drive_t scsi_cd_drive_applecd_24x = {
    .id = "applecd24x",
    .name = "AppleCD 24X",
    .vendor = "MATSHITA",
    .product = "CD-ROM CR-8024",
    .revision = "2.0e",
    .block_size = 2048,
    .family = &cd_family_apple_scsi2_tray,
};

const scsi_cd_drive_t *const scsi_cd_drives[] = {
    &scsi_cd_drive_applecd_sc_plus, &scsi_cd_drive_applecd_300i,      &scsi_cd_drive_applecd_300i_plus,
    &scsi_cd_drive_applecd_600i,    &scsi_cd_drive_applecd_600i_sony, &scsi_cd_drive_applecd_1200i,
    &scsi_cd_drive_applecd_1600i,   &scsi_cd_drive_applecd_24x,       NULL,
};

// Do `a` and the padded field `b` hold the same string, ignoring trailing
// spaces on either?
static bool same_padded(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    while (la > 0 && a[la - 1] == ' ')
        la--; // drop the padding
    while (lb > 0 && b[lb - 1] == ' ')
        lb--;
    return la == lb && memcmp(a, b, la) == 0;
}

const scsi_cd_drive_t *scsi_cd_drive_find(const char *id) {
    for (int i = 0; id && scsi_cd_drives[i]; i++)
        if (strcmp(scsi_cd_drives[i]->id, id) == 0)
            return scsi_cd_drives[i];
    return NULL;
}

const scsi_cd_drive_t *scsi_cd_drive_by_identity(const char *vendor, const char *product, const char *revision) {
    if (!vendor || !product || !revision)
        return NULL;
    for (int i = 0; scsi_cd_drives[i]; i++) {
        const scsi_cd_drive_t *d = scsi_cd_drives[i];
        if (same_padded(d->vendor, vendor) && same_padded(d->product, product) && same_padded(d->revision, revision))
            return d;
    }
    return NULL;
}

const scsi_cd_drive_t *scsi_cd_drive_at(const scsi_t *bus, unsigned which) {
    if (!bus || which >= 8 || bus->devices[which].type != scsi_dev_cdrom)
        return NULL;
    return bus->cd_drive[which];
}

const scsi_cd_family_t *scsi_cd_family_at(const scsi_t *scsi, int target) {
    const scsi_cd_drive_t *d = scsi_cd_drive_at(scsi, (unsigned)target);
    return d && d->family ? d->family : &cd_family_sony_scsi1;
}

// ============================================================================
// MODE SENSE Pages
// ============================================================================

// How long each piece of a MODE SENSE response is.
//
// Named rather than repeated as literals because the total has to be a
// compile-time bound on the response buffer, and a page that grows without the
// buffer growing with it is a stack overflow in a build where assertions are
// compiled out (GS_FAST, the shipping wasm profile).  Each builder returns its
// own constant, so a page cannot change length without changing the sum.
#define CD_MODE_PARAM_HEADER_LEN 4
#define CD_MODE_BLOCK_DESC_LEN   8
#define CD_PAGE_01_LEN           8
#define CD_PAGE_02_LEN           16
#define CD_PAGE_07_LEN           8
#define CD_PAGE_08_LEN           12
#define CD_PAGE_09_LEN           16
// Page 30h is a 2-byte page header plus a 30-byte body.
#define CD_PAGE_30_BODY 30
#define CD_PAGE_30_LEN  (2 + CD_PAGE_30_BODY)

// MODE SENSE(10), the form an ATAPI host uses: an 8-byte header, and the CD
// capabilities page 2Ah (SFF-8020i), 2 + 18 bytes.
#define CD_MODE_PARAM_HEADER10_LEN 8
#define CD_PAGE_2A_LEN             20

// The longest response this file can build: page code 3Fh, "all pages",
// in the 10-byte form (no block descriptor there, but count it anyway).
#define CD_MODE_SENSE_MAX                                                                                              \
    (CD_MODE_PARAM_HEADER10_LEN + CD_MODE_BLOCK_DESC_LEN + CD_PAGE_01_LEN + CD_PAGE_02_LEN + CD_PAGE_07_LEN +          \
     CD_PAGE_08_LEN + CD_PAGE_09_LEN + CD_PAGE_30_LEN + CD_PAGE_2A_LEN)

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
        return CD_PAGE_01_LEN;
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
    return CD_PAGE_01_LEN;
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
        return CD_PAGE_07_LEN; // nothing changeable: header, zero body
    buf[2] = 0x00; // error recovery parameter
    buf[3] = 0x00; // verify retry count -- "the same as for read operations"
    return CD_PAGE_07_LEN;
}

// Build Mode Page 0x02: Disconnect-Reconnect Parameters (16 bytes)
static int build_page_02(uint8_t *buf) {
    buf[0] = 0x02; // page code
    buf[1] = 0x0E; // page length
    memset(buf + 2, 0, 14); // all zeros (we don't disconnect)
    return CD_PAGE_02_LEN;
}

// Build Mode Page 0x08: Caching Parameters (12 bytes)
static int build_page_08(uint8_t *buf) {
    buf[0] = 0x08; // page code
    buf[1] = 0x0A; // page length
    memset(buf + 2, 0, 10);
    return CD_PAGE_08_LEN;
}

// Build Mode Page 0x09: Audio Control Parameters (16 bytes)
static int build_page_09(uint8_t *buf, int page_control) {
    buf[0] = 0x09; // page code
    buf[1] = 0x0E; // page length
    memset(buf + 2, 0, 14);
    if (page_control == 1)
        return CD_PAGE_09_LEN; // nothing here is changeable: header, zero body
    // Output port 0 channel selection = 01 (left)
    buf[8] = 0x01;
    buf[9] = 0xFF; // volume
    // Output port 1 channel selection = 02 (right)
    buf[10] = 0x02;
    buf[11] = 0xFF; // volume
    return CD_PAGE_09_LEN;
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
    return scsi_build_apple_page_30(buf, page_control, apple_cd_id, (int)sizeof(apple_cd_id) - 1, CD_PAGE_30_BODY);
}

// Mode page 2Ah, CD Capabilities and Mechanical Status (SFF-8020i; MMC):
// a read-only CD-ROM in a tray that locks and ejects, with audio play, at
// a 24x read rate.  An ATAPI driver reads it to learn what the drive is;
// nothing in it is changeable.
static int build_page_2a(uint8_t *buf, int page_control) {
    memset(buf, 0, CD_PAGE_2A_LEN);
    buf[0] = 0x2A; // page code
    buf[1] = CD_PAGE_2A_LEN - 2; // page length
    if (page_control == 1)
        return CD_PAGE_2A_LEN; // changeable: nothing
    buf[4] = 0x01; // audio play
    buf[5] = 0x01; // CD-DA commands supported
    buf[6] = 0x29; // tray loading (001b), eject, lock
    buf[8] = 0x10; // maximum read speed 4234 KB/s (24x)
    buf[9] = 0x8A;
    buf[10] = 0x01; // 256 volume levels
    buf[11] = 0x00;
    buf[12] = 0x00; // 128 KB buffer
    buf[13] = 0x80;
    buf[14] = 0x10; // current read speed
    buf[15] = 0x8A;
    return CD_PAGE_2A_LEN;
}

// ============================================================================
// MODE SENSE(6) / MODE SENSE(10) Handler
// ============================================================================

// The two forms differ in the header: MODE SENSE(10) has an 8-byte one with
// two-byte length fields, and answers without a block descriptor, so the
// first page starts at offset 8 where an ATAPI driver looks for it; and it
// knows page 2Ah, which a SCSI-1 CDU-8002 has no notion of.
static void cd_mode_sense(scsi_t *scsi, bool ten) {
    int target = scsi->bus.target & 7;
    uint16_t alloc_len = ten ? (uint16_t)(scsi->buf.data[7] << 8 | scsi->buf.data[8]) : scsi->buf.data[4];
    uint8_t page_code = scsi->buf.data[2] & 0x3F;
    int page_control = (scsi->buf.data[2] >> 6) & 0x03;

    // Assemble into a scratch buffer and copy after the phase is armed, which
    // is what the four other data-in builders in this file and the hard disk's
    // MODE SENSE all do.
    //
    // This used to build straight into scsi->buf.data -- the live transfer
    // buffer, while it still held the CDB being parsed -- after memset-ing the
    // whole BUF_LIMIT region, 131072 bytes, on every call.  It worked, but for
    // none of the reasons its comment gave.  That comment said zeroing the tail
    // stopped "stale payload from a previous command" leaking, and no tail can
    // leak: scsi_data_in_alloc sets buf.size to min(pos, alloc_len), next_byte
    // counts it down, and every drain stops at zero, so nothing past `pos` is
    // ever transmitted.  Of the bytes that ARE sent, every page zeroes its own
    // body and the block descriptor writes all eight of its bytes -- exactly
    // two, the header's medium type and device-specific parameter, depended on
    // that 128 KB memset, and sizeof(resp) covers them for free.
    uint8_t resp[CD_MODE_SENSE_MAX];
    memset(resp, 0, sizeof(resp));
    uint8_t *buf = resp;

    // Mode parameter header
    int pos = ten ? CD_MODE_PARAM_HEADER10_LEN : CD_MODE_PARAM_HEADER_LEN;

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

    if (ten) {
        buf[2] = 0x01; // medium type: 120 mm CD-ROM data disc
        // Block descriptor length (bytes 6-7) stays 0: none follows.
    } else {
        buf[3] = CD_MODE_BLOCK_DESC_LEN; // block descriptor length
        buf[pos + 0] = 0; // density code
        buf[pos + 1] = (blocks >> 16) & 0xFF; // number of blocks
        buf[pos + 2] = (blocks >> 8) & 0xFF;
        buf[pos + 3] = blocks & 0xFF;
        buf[pos + 4] = 0; // reserved
        buf[pos + 5] = (reported_blk_sz >> 16) & 0xFF; // block length, per PC above
        buf[pos + 6] = (reported_blk_sz >> 8) & 0xFF;
        buf[pos + 7] = reported_blk_sz & 0xFF;
        pos += CD_MODE_BLOCK_DESC_LEN;
    }

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
    case 0x2A:
        if (!ten) {
            scsi_check_condition(scsi, SENSE_ILLEGAL_REQUEST, ASC_INVALID_FIELD_IN_CDB, 0x00);
            return;
        }
        pos += build_page_2a(buf + pos, page_control);
        break;
    case 0x3F:
        // Return all pages
        pos += build_page_01(buf + pos, page_control);
        pos += build_page_02(buf + pos);
        pos += build_page_07(buf + pos, page_control);
        pos += build_page_08(buf + pos);
        pos += build_page_09(buf + pos, page_control);
        pos += build_page_30(buf + pos, page_control);
        if (ten)
            pos += build_page_2a(buf + pos, page_control);
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

    // Nothing above may have run past the buffer.  CD_MODE_SENSE_MAX is summed
    // from the same constants the builders return, so this cannot fail without
    // someone having changed one and not the other -- but it is cheap, and the
    // build where it would matter most is the one where assertions are gone.
    GS_ASSERTF(pos <= (int)sizeof(resp), "MODE SENSE built %d bytes into a %zu-byte buffer", pos, sizeof(resp));

    // Fill in the mode data length (byte 0 = total length - 1). The field is
    // a single byte; the longest response this can build is
    // CD_MODE_SENSE_MAX, well inside that, so the old clamp to 255 guarded
    // against a size the constants now make impossible.
    if (ten) {
        buf[0] = (uint8_t)((pos - 2) >> 8); // mode data length, excluding itself
        buf[1] = (uint8_t)(pos - 2);
    } else {
        buf[0] = (uint8_t)(pos - 1);
    }

    // Bound by the allocation length.  Zero means zero -- CDU-541 manual
    // S4.2.6 -- where this used to read `alloc_len > 0 &&`, i.e. send the whole
    // response to a probe that allocated nothing for it.
    scsi_data_in_copy(scsi, resp, pos, alloc_len);
}

// Handle MODE SENSE(6) for CD-ROM device
void scsi_cdrom_mode_sense(scsi_t *scsi) {
    cd_mode_sense(scsi, false);
}

// Handle MODE SENSE(10) for CD-ROM device
void scsi_cdrom_mode_sense_10(scsi_t *scsi) {
    cd_mode_sense(scsi, true);
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

    // Parse the block descriptor if present.  The CDU-541 manual S5.2.2 lists
    // six block lengths this drive accepts (Table 5-4): 256, 512, 1024, 2048,
    // 2336 and 2340.  "Any other value will be considered an error.  The
    // command will be terminated with a CHECK CONDITION status.  The sense key
    // is set to ILLEGAL REQUEST and the additional sense code is set to INVALID
    // FIELD IN PARAMETER LIST."
    //
    // Two of the six are implemented -- 2048, the Mode 1 sector this drive
    // serves by default, and 512, which is what A/UX switches its install disc
    // to (se30-aux-3).  The other four are legal requests this emulator cannot
    // honour, so they go to GS_UNIMPLEMENTED rather than being answered:
    // silently keeping the old size, which is what this did, tells the guest
    // the switch happened and then serves it the wrong sectors.
    if (bd_len >= 8 && offset + 8 <= len) {
        uint32_t block_len =
            ((uint32_t)data[offset + 5] << 16) | ((uint32_t)data[offset + 6] << 8) | (uint32_t)data[offset + 7];
        switch (block_len) {
        case 0:
            // A zero-filled descriptor is how a driver says "I came here for
            // the pages, not the medium".  Strictly the manual's "any other
            // value" covers it -- zero is not in Table 5-4, and X3.131-1994
            // S8.3.3 defines a zero block length only for SEQUENTIAL-access
            // devices, which this is not -- but refusing a legal MODE SELECT
            // over a field the initiator left blank is the worse error.  Same
            // reading on the hard disk path, so the two agree.
            break;
        case 512:
        case 2048:
            scsi->devices[target].block_size = (uint16_t)block_len;
            break;
        case 256:
        case 1024:
        case 2336:
        case 2340:
            GS_UNIMPLEMENTED("MODE SELECT asked for a %u-byte CD-ROM block; the CDU-541 supports it "
                             "(Table 5-4) but only 512 and 2048 are modelled",
                             block_len);
            break;
        default:
            // Not one the drive has ever offered: the guest is wrong, and the
            // manual says exactly how to say so.
            scsi_check_condition(scsi, SENSE_ILLEGAL_REQUEST, ASC_INVALID_FIELD_IN_PARAM_LIST, 0x00);
            return;
        }
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

// READ TOC formats 01b and 10b for a single-session data disc (SFF-8020i
// r2.6 §10.8.19, Tables 127 and 128).  01b: the first and last session (1)
// and session 1's first track, at LBA 0.  10b: the session's lead-in
// Q sub-channel entries -- points A0h (first track 1, disc type 00h,
// CD-ROM), A1h (last track 1) and A2h (the lead-out's start), then track 1 --
// as 11-byte descriptors whose addresses are always MSF.
static void scsi_cdrom_read_toc_session(scsi_t *scsi, uint8_t format, bool msf, uint16_t alloc_len) {
    int target = scsi->bus.target & 7;
    uint16_t blk_sz = scsi->devices[target].block_size;
    uint32_t total = 0;
    if (scsi->device_images[target])
        total = (uint32_t)(disk_size(scsi->device_images[target]) / blk_sz);

    uint8_t toc[4 + 4 * 11];
    memset(toc, 0, sizeof(toc));
    int pos = 4;
    if (format == 1) {
        toc[pos + 1] = 0x14; // ADR=1, control=4 (data track)
        toc[pos + 2] = 0x01; // first track in the last session
        put_cd_address(&toc[pos + 4], 0, msf);
        pos += 8;
    } else {
        static const uint8_t points[4] = {0xA0, 0xA1, 0xA2, 0x01};
        for (int i = 0; i < 4; i++) {
            uint8_t *d = &toc[pos];
            d[0] = 0x01; // session
            d[1] = 0x14; // ADR=1, control=4
            d[3] = points[i];
            uint8_t a[4];
            if (points[i] == 0xA0) {
                d[8] = 0x01; // first track
            } else if (points[i] == 0xA1) {
                d[8] = 0x01; // last track
            } else {
                lba_to_msf(points[i] == 0xA2 ? total : 0, a);
                d[8] = a[1], d[9] = a[2], d[10] = a[3];
            }
            pos += 11;
        }
    }
    toc[0] = (uint8_t)((pos - 2) >> 8);
    toc[1] = (uint8_t)(pos - 2);
    toc[2] = 0x01; // first session
    toc[3] = 0x01; // last session
    scsi_data_in_copy(scsi, toc, pos, alloc_len);
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

    // An ATAPI host asks for the multi-session (01b) and full (10b) TOC
    // formats: SFF-8020i r2.6 §10.8.19 Table 125 puts the format in byte 2
    // bits 3-0 (MMC) and, when that is zero, in byte 9 bits 7-6 -- a field
    // vendor-specific under SCSI-2 and zero from every SCSI-2 driver, so the
    // SCSI path still gets format 00b below.  Mac OS X's CD driver reads the
    // full TOC to build the disc's tracks; answered in format 00b layout it
    // published no medium and the boot waited for its root device.
    uint8_t format = scsi->buf.data[2] & 0x0F;
    if (!format)
        format = scsi->buf.data[9] >> 6;
    if (format == 1 || format == 2) {
        scsi_cdrom_read_toc_session(scsi, format, msf, alloc_len);
        return;
    }
    if (format > 2) {
        scsi_check_condition(scsi, SENSE_ILLEGAL_REQUEST, ASC_INVALID_FIELD_IN_CDB, 0x00);
        return;
    }

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

    scsi_data_in_copy(scsi, toc, pos, alloc_len);
}

// Binary to packed BCD, for the vendor TOC's track numbers and MSF fields.
static uint8_t to_bcd(uint32_t v) {
    return (uint8_t)(((v / 10u) % 10u) << 4 | (v % 10u));
}

// One absolute CD address as three BCD bytes M, S, F (the +150 two-second
// lead-in, as lba_to_msf).
static void put_bcd_msf(uint8_t *dst, uint32_t lba) {
    uint8_t msf[4];
    lba_to_msf(lba, msf);
    dst[0] = to_bcd(msf[1]);
    dst[1] = to_bcd(msf[2]);
    dst[2] = to_bcd(msf[3]);
}

// Handle the vendor READ TOC (C1h) of the AppleCD SC's CDU-8002, the command
// System 7's Apple CD-ROM driver uses for the disc's table of contents.  It is
// not the SCSI-2 (43h) layout.  The control byte's bits 7-6 (CDB byte 9) pick
// what comes back, the allocation length is bytes 7-8, and every answer is
// built from 4-byte BCD fields:
//   00b  first and last track numbers:   [first][last][0][0]
//   01b  start of the lead-out:          [M][S][F][0]
//   10b  from the track in byte 5, one entry per 4 bytes requested:
//        [ADR/control][M][S][F]; entries past the last track repeat it
//   11b  reserved
// The driver asks 00b, then 01b, then 10b from the first track it was told
// and classifies each track by its control nibble.  This used to answer
// every request with a header and 6-byte descriptors, whatever bits 7-6 said:
// the driver read the header as the track range and a zero as track 1's
// control -- an audio track -- so a plain ISO 9660 disc mounted as
// "Audio CD 1".
void scsi_cdrom_read_toc_sony(scsi_t *scsi) {
    int target = scsi->bus.target & 7;
    uint16_t alloc_len = (uint16_t)((scsi->buf.data[7] << 8) | scsi->buf.data[8]);
    uint8_t type = scsi->buf.data[9] & 0xC0;

    // Single data session: track 1 at LBA 0, the lead-out after the last block.
    uint16_t blk_sz = scsi->devices[target].block_size;
    uint32_t total = 0;
    if (scsi->device_images[target])
        total = (uint32_t)(disk_size(scsi->device_images[target]) / blk_sz);

    uint8_t toc[256];
    memset(toc, 0, sizeof(toc));
    int len = 4;
    switch (type) {
    case 0x00: // the track range
        toc[0] = to_bcd(1); // first track
        toc[1] = to_bcd(1); // last track
        break;
    case 0x40: // the lead-out
        put_bcd_msf(&toc[0], total);
        break;
    case 0x80: { // track entries from the starting track (byte 5, BCD)
        len = alloc_len < sizeof(toc) ? (alloc_len & ~3) : (int)sizeof(toc);
        for (int pos = 0; pos < len; pos += 4) {
            toc[pos] = 0x14; // ADR=1, control=4: data track (the only track)
            put_bcd_msf(&toc[pos + 1], 0); // track 1 starts at LBA 0
        }
        break;
    }
    default: // 11b is reserved
        scsi_check_condition(scsi, SENSE_ILLEGAL_REQUEST, ASC_INVALID_FIELD_IN_CDB, 0x00);
        return;
    }
    scsi_data_in_copy(scsi, toc, len, alloc_len);
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

    scsi_data_in_copy(scsi, resp, 4, alloc_len);
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

    scsi_data_in_copy(scsi, resp, 8, alloc_len);
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
        //
        // That is the Sony vocabulary; a SCSI-2 tray drive reports 53h/02h
        // MEDIUM REMOVAL PREVENTED instead, so the code is the drive family's.
        if (scsi_eject_device(scsi, target) == -2) {
            const scsi_cd_family_t *fam = scsi_cd_family_at(scsi, target);
            scsi_check_condition(scsi, SENSE_ILLEGAL_REQUEST, fam->removal_prevented_asc, fam->removal_prevented_ascq);
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
    // the appropriate code for an empty bay is 0xB0 -- on a Sony; each drive
    // family names its own (scsi_check_no_medium).
    //
    // ALLOW is not covered by that sentence and is not refused: unlocking a
    // drive that has nothing in it is harmless, and a driver tidying up after
    // an eject has every reason to send it.
    if (prevent && !scsi->devices[target].medium_present) {
        scsi_check_no_medium(scsi, target);
        return;
    }

    scsi->devices[target].prevent_removal = prevent;
    phase_status(scsi, STATUS_GOOD);
}
