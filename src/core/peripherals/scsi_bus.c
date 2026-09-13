// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// scsi_bus.c
// The SCSI bus itself: the phase machine, the eight target slots and the
// device models that answer on them, the staging buffer, the command layer,
// and the initiator-facing API every controller drives it through.
//
// This is deliberately NOT a chip.  Four controllers reach this one bus -- the
// NCR 5380 (scsi.c), the NCR 53C96 (scsi_53c96.c), the Symbios 53C825 SCRIPTS
// engine (pci/cards/) and Apple's MESH (machines/tnt/) -- and none of them
// carries a copy of any of it.  Before this file existed the bus lived inside
// the 5380's translation unit, which had two consequences worth recording so
// they are not reintroduced:
//
//   - The 5380 was not a client of the bus, it WAS the bus, reaching into
//     bus.phase and buf directly ~90 times while the other three could only go
//     through the initiator API.  A chip that owns the wire everyone shares is
//     a chip whose quirks become everyone's.
//
//   - "Reset the bus" had no home, so it grew four different bodies.  The only
//     entry point the bus exposed was scsi_reset_pin(), which resets a 5380
//     register file, so a Power Macintosh with no 5380 anywhere still called
//     it, and the two front-ends that declined hand-rolled partial resets
//     instead.  scsi_bus_reset() below is the one the wire actually implies.
//
// Anything that is true of the WIRE or of a TARGET belongs here.  Anything
// that is true of one chip's register file belongs with that chip.

#include "scsi.h"

#include "drive_catalog.h"
#include "image.h"
#include "log.h"
#include "object.h"
#include "platform.h"
#include "scheduler.h"
#include "scsi_internal.h"
#include "shell.h"
#include "system.h"
#include "system_config.h"
#include "value.h"

LOG_USE_CATEGORY_NAME("scsi");

#include <assert.h>
#include <stdlib.h>
#include <string.h>

// Determine the length of a SCSI command from its group code -- the top three
// bits of the opcode.
//
// ANSI X3.131-1986 (SCSI-1) section 6.2.1 defines groups 0, 1 and 5 as six-,
// ten- and twelve-byte commands and leaves groups 2, 3 and 4 reserved.  The
// Am53C94 datasheet (STATREG bit 3, "Group Code Valid") documents how a real
// target of this era sizes the groups the standard left open, and that is what
// we follow:
//
//   group 0  $00-$1F   6   SCSI-1.
//   group 1  $20-$3F  10   SCSI-1.
//   group 2  $40-$5F  10   SCSI-2.  The 53C94 does this only with its S2FE bit
//                              set, but the CD-ROM audio commands we implement
//                              ($42 READ SUB-CHANNEL through $4B PAUSE/RESUME)
//                              live here, so for us it is unconditional.
//   group 3  $60-$7F   6   Reserved; the chip treats reserved groups as
//                              six-byte commands.
//   group 4  $80-$9F   6   Reserved, likewise.  Sixteen-byte group 4 commands
//                              are a SCSI-3 invention, later than any machine
//                              or drive we model.
//   group 5  $A0-$BF  12   SCSI-1.
//   group 6  $C0-$DF  10   Vendor unique.  The chip guesses six, but the device
//                              defines the true length and ours is a Sony
//                              CDU-541, whose vendor commands are ten-byte CDBs
//                              (CDU-541 SCSI manual section 5.2.23: READ TOC
//                              $C1 runs byte 0 through byte 9).
//   group 7  $E0-$FF  10   Vendor unique; "always treated as ten byte".
//
// This is not a cosmetic table.  run_cmd() fires the instant the accumulated
// byte count matches, so an undersized answer dispatches the command early and
// spills the tail of the CDB into whichever phase follows.  An opcode we do not
// implement still has to be *counted* correctly, so that run_cmd can decline it
// with ILLEGAL REQUEST / INVALID OPCODE instead of corrupting the next phase.
int cmd_size(uint8_t opcode) {
    switch (opcode >> 5) {
    case 0:
        return 6;
    case 1:
        return 10;
    case 2:
        return 10;
    case 3:
        return 6;
    case 4:
        return 6;
    case 5:
        return 12;
    case 6:
        return 10;
    default:
        return 10; // group 7
    }
}

// Ensure the staging buffer can hold at least `bytes`.  The buffer starts at
// BUF_LIMIT and grows (never shrinks) so a single READ/WRITE larger than 256
// blocks — e.g. the Apple SCSI driver's multi-block writes during a System 7.1
// install — is staged whole rather than tripping a fixed-size assert.  The bus
// handshake is still byte-by-byte, so this is invisible to the guest; only the
// host-side staging area changes size.
void scsi_buf_ensure(scsi_t *scsi, size_t bytes) {
    if (bytes <= scsi->buf.cap)
        return;
    uint8_t *grown = realloc(scsi->buf.data, bytes);
    GS_ASSERTF(grown != NULL, "scsi_buf_ensure: failed to grow transfer buffer to %zu bytes", bytes);
    scsi->buf.data = grown;
    scsi->buf.cap = bytes;
}

// Pop the next byte from the SCSI buffer.  Data-in is drained front-to-back via
// a read cursor (buf.pos) rather than memmove-ing the remainder down on every
// byte — the latter is O(n^2) and stalls multi-hundred-KB transfers.  buf.size
// still tracks remaining bytes so every "drained" (size == 0) check is unchanged.
uint8_t next_byte(scsi_t *scsi) {
    assert(scsi->buf.size > 0);

    uint8_t byte = scsi->buf.data[scsi->buf.pos++];
    scsi->buf.size--;

    return byte;
}

// Transition SCSI bus to the free/idle state
void phase_free(scsi_t *scsi) {
    scsi->bus.phase = scsi_bus_free;
    scsi->bus.req = false;
    scsi->bus.bsy = false;
    scsi_5380_bus_freed(scsi);
    scsi_cancel_drq_service(scsi);
    scsi_update_drq(scsi);
    scsi_update_irq(scsi);
}

// A guest drives every one of these transitions through the 5380 register
// file, so "the bus is in the phase this transition starts from" is not an
// invariant the model can assume -- it is a request that may be malformed.
// Declining is what the rest of the file already does (see the CHECK CONDITION
// comment on the INQUIRY path: "the guest may legitimately try").
//
// These used to be assert()s.  That was wrong in both directions: the default
// headless build keeps assertions live (Makefile.headless: "No -DNDEBUG"), so
// a guest could abort CI with five byte-writes to one register, while the
// GS_FAST/NDEBUG builds compiled the check out and walked on regardless.  A
// logged early return is the same behaviour in every build mode.
#define PHASE_REQUIRE(scsi, cond)                                                                                      \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            LOG(1, "scsi: %s declined from phase %s (guest drove an out-of-order transition)", __func__,               \
                phase_name((scsi)->bus.phase));                                                                        \
            return;                                                                                                    \
        }                                                                                                              \
    } while (0)

const char *const SCSI_PHASE_NAMES[] = {
    "bus_free", "arbitration", "selection", "reselection", "command",
    "data_in",  "data_out",    "status",    "message_in",  "message_out",
};

const char *phase_name(int p) {
    return (p >= 0 && p < (int)(sizeof(SCSI_PHASE_NAMES) / sizeof(SCSI_PHASE_NAMES[0]))) ? SCSI_PHASE_NAMES[p] : "?";
}

// Transition SCSI bus to arbitration phase
void phase_arbitration(scsi_t *scsi) {
    PHASE_REQUIRE(scsi, scsi->bus.phase == scsi_bus_free);

    scsi->bus.phase = scsi_arbitration;
}

// Transition SCSI bus to selection phase (from arbitration or bus-free for non-arbitrated selection)
void phase_selection(scsi_t *scsi) {
    PHASE_REQUIRE(scsi, scsi->bus.phase == scsi_arbitration || scsi->bus.phase == scsi_bus_free);

    scsi->bus.phase = scsi_selection;
}

// Transition SCSI bus to command phase
void phase_command(scsi_t *scsi) {
    PHASE_REQUIRE(scsi, scsi->bus.phase == scsi_selection);

    // by not asserting MSG, we indicate that we don't support any messages (other than command complete)
    // i.e. go directly to the command phase
    scsi->bus.req = scsi->bus.bsy = true;

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
    scsi->bus.req = scsi->bus.bsy = true;
    scsi_buf_ensure(scsi, (size_t)bytes);
    scsi->buf.size = scsi->buf.max = bytes;
    scsi->buf.pos = 0; // fresh fill: deliver from the front
    // Skip scsi_update_irq: prevents spurious phase-mismatch IRQ when
    // run_cmd fires during pseudo-DMA ODR write with MR_DMA still set
    // for command phase.
}

// Arm DATA IN for a response bounded by the CDB's allocation length.  See the
// declaration in scsi_internal.h for why zero means zero.
int scsi_data_in_alloc(scsi_t *scsi, int have, int alloc) {
    if (alloc < 0)
        alloc = 0;
    int len = alloc < have ? alloc : have;
    if (len <= 0) {
        // Nothing to transfer: no DATA IN phase at all, GOOD status.  Matches
        // what MODE SELECT already does for a zero parameter-list length, and
        // avoids parking the bus in DATA IN with bytes the initiator never
        // allocated for -- every exit from DATA IN is guarded by buf.size == 0,
        // so a phase armed with data nobody drains does not leave on its own.
        phase_status(scsi, STATUS_GOOD);
        return 0;
    }
    phase_data_in(scsi, len);
    return len;
}

// Transition SCSI bus to data-out phase (initiator to target)
void phase_data_out(scsi_t *scsi, int bytes) {
    assert(scsi->bus.phase == scsi_command);

    scsi->bus.phase = scsi_data_out;
    scsi->bus.req = scsi->bus.bsy = true;
    scsi_buf_ensure(scsi, (size_t)bytes);
    scsi->buf.max = bytes;
    scsi->buf.size = 0;
    // Entering DATA OUT resets a 5380's priming state, if there is one.
    scsi_5380_entered_data_out(scsi);
}

// Transition SCSI bus to message-out phase (initiator to target).
// SCSI targets enter this phase when the initiator asserts ATN,
// allowing the initiator to send messages such as IDENTIFY or ABORT.
void phase_message_out(scsi_t *scsi) {
    scsi->bus.saved_phase = scsi->bus.phase;
    scsi->bus.phase = scsi_message_out;
    // MSG + C/D + REQ + BSY, no I/O (direction is initiator → target)
    scsi->bus.req = scsi->bus.bsy = true;
    scsi_update_irq(scsi);
}

// Transition SCSI bus to status phase
void phase_status(scsi_t *scsi, uint8_t status) {
    assert(scsi->bus.phase == scsi_command || scsi->bus.phase == scsi_data_in || scsi->bus.phase == scsi_data_out ||
           scsi->bus.phase == scsi_message_out);

    bool was_data_in = (scsi->bus.phase == scsi_data_in);

    scsi->bus.phase = scsi_status;
    scsi->bus.req = scsi->bus.bsy = true;
    scsi->bus.data = status;

    // Entering STATUS is a wire event; what a controller makes of it is its
    // own business.  The 5380 latches end-of-DMA here and, coming out of DATA
    // IN under DMA, deliberately withholds the interrupt -- reasoning that
    // belongs with the chip and used to sit in this function, reading the
    // chip's mode register to get it.
    scsi_5380_entered_status(scsi, was_data_in);
}

// Transition SCSI bus to message-in phase
void phase_message_in(scsi_t *scsi, uint8_t message) {
    assert(scsi->bus.phase == scsi_status);

    scsi->bus.phase = scsi_message_in;
    scsi->bus.req = scsi->bus.bsy = true;
    scsi->bus.data = message;
    scsi_update_irq(scsi);
}

// Set sense data for a device
void scsi_set_sense(scsi_t *scsi, int target, uint8_t key, uint8_t asc, uint8_t ascq) {
    scsi->devices[target & 7].sense.key = key;
    scsi->devices[target & 7].sense.asc = asc;
    scsi->devices[target & 7].sense.ascq = ascq;
}

// Does this opcode touch the medium, as opposed to the drive itself?  An
// empty CD bay must answer the drive-level commands normally — that is how a
// guest tells an empty drive from a broken one — and fail only the ones that
// need a disc.  INQUIRY, REQUEST SENSE, MODE SENSE/SELECT, START/STOP,
// PREVENT/ALLOW, RESERVE/RELEASE and the diagnostics are all drive-level.
static bool scsi_cmd_needs_medium(uint8_t opcode) {
    switch (opcode) {
    case CMD_REZERO_UNIT:
    case CMD_FORMAT_UNIT:
    case CMD_READ:
    case CMD_WRITE:
    case CMD_SEEK_6:
    case CMD_READ_CAPACITY:
    case CMD_READ_10:
    case CMD_WRITE_10:
    case CMD_SEEK_10:
    case CMD_WRITE_VERIFY:
    case CMD_VERIFY:
    case CMD_READ_SUB_CHANNEL:
    case CMD_READ_TOC:
    case CMD_READ_HEADER:
    case CMD_PLAY_AUDIO_10:
    case CMD_PLAY_AUDIO_MSF:
    case CMD_PAUSE_RESUME:
    case CMD_SONY_READ_TOC:
    case CMD_SONY_PLAYBACK_STATUS:
    case CMD_SONY_PAUSE:
    case CMD_SONY_PLAY_TRACK:
    case CMD_SONY_PLAY_MSF:
    case CMD_SONY_PLAY_AUDIO:
    case CMD_SONY_PLAYBACK_CTRL:
        return true;
    default:
        return false;
    }
}

// Return CHECK CONDITION, setting sense data on the current target
void scsi_check_condition(scsi_t *scsi, uint8_t sense_key, uint8_t asc, uint8_t ascq) {
    scsi_set_sense(scsi, scsi->bus.target, sense_key, asc, ascq);
    phase_status(scsi, STATUS_CHECK_CONDITION);
}

// Does the command's [lba, lba + tl) block range fit inside the medium?
//
// One answer for READ and WRITE on both CDB lengths.  WRITE had none at all:
// its only guards were two assert()s in command_complete, and assert is
// compiled out by -DNDEBUG in the release wasm profile (Makefile:131), so an
// out-of-range WRITE reached disk_write_data, which drops the unbacked tail --
// and the SCSI layer then reported STATUS GOOD.  Silent data loss reported as
// success (03-scsi F-02).
//
// Everything is computed in uint64_t because size_t is 32 bits on wasm32,
// where `(size_t)lba * blk_sz` wraps: a READ(10) at lba 0x00400000 with
// 2048-byte blocks gives 0x800000000, which truncates to 0, so the old check
// passed and the wrong blocks were served as valid data (03-scsi F-04).
static bool scsi_lba_range_ok(const scsi_t *scsi, int target, size_t *off_out, size_t *cnt_out) {
    const image_t *img = scsi->device_images[target];
    if (!img)
        return false;
    // Through uint32_t first: cmd.lba and cmd.tl are `int`, and the 10-byte
    // decode builds them with `data[2] << 24`, which overflows a signed int for
    // any byte >= 0x80.  A CDB of FF FF FF FF lands as -1, and casting that
    // straight to uint64_t sign-extends to 0xFFFF...FFFF, whose product with
    // the block size wraps and passes any bound.  (Found by the scsi_bounds
    // unit test, against the first version of THIS function.)
    uint64_t blk = scsi->devices[target].block_size;
    uint64_t off = (uint64_t)(uint32_t)scsi->cmd.lba * blk;
    uint64_t cnt = (uint64_t)(uint32_t)scsi->cmd.tl * blk;
    if (off + cnt > (uint64_t)img->raw_size)
        return false;
    // Both are bounded by raw_size above, so narrowing is safe.
    if (off_out)
        *off_out = (size_t)off;
    if (cnt_out)
        *cnt_out = (size_t)cnt;
    return true;
}

// Execute a SCSI command after receiving it from the initiator
void run_cmd(scsi_t *scsi) {
    scsi->cmd.opcode = scsi->buf.data[0];
    // WRITE AND VERIFY(10) (SCSI-2 §9.2.22) is WRITE(10) plus a medium
    // verification that cannot fail against a disk image, and its CDB has
    // the same layout — canonicalise it so every WRITE_10 path (dispatch,
    // completion, read-only rejection) handles it.  AIX's LVM writes its
    // bad-block directory with it and declares the directory "corrupted"
    // when the command is refused.
    if (scsi->cmd.opcode == CMD_WRITE_VERIFY)
        scsi->cmd.opcode = CMD_WRITE_10;
    int target = scsi->bus.target & 7;
    // The command block as it arrived.  Most opcodes below say nothing at
    // all unless something goes wrong, which is exactly backwards when the
    // question is "what did the guest ask for?" — the answer a guest's
    // driver hung on is usually the command before the one that failed.
    LOG(4, "CDB target=%d: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X", target, scsi->buf.data[0],
        scsi->buf.data[1], scsi->buf.data[2], scsi->buf.data[3], scsi->buf.data[4], scsi->buf.data[5],
        scsi->buf.data[6], scsi->buf.data[7], scsi->buf.data[8], scsi->buf.data[9]);

    // Pending UNIT ATTENTION, reported on the first command that is not exempt.
    //
    // Three commands are exempt, per the CDU-541 manual 4.1.3: INQUIRY and
    // REQUEST SENSE (also ANSI X3.131-1986 6.1.3), plus STOP UNIT with LoEj
    // set -- "the controller will perform the command and will not clear the
    // unit attention condition".  That third one is a Sony extension; the ANSI
    // text has no such carve-out.  Without it, ejecting a disc that was only
    // just inserted fails, because the insert's own UNIT ATTENTION is still
    // pending and swallows the eject.
    bool stop_unit_eject = scsi->cmd.opcode == CMD_START_STOP_UNIT && (scsi->buf.data[4] & 0x03) == 0x02;
    if (scsi->devices[target].unit_attention && scsi->cmd.opcode != CMD_INQUIRY &&
        scsi->cmd.opcode != CMD_REQUEST_SENSE && !stop_unit_eject) {
        scsi->devices[target].unit_attention = false;
        // Report the condition that was staged when the attention was raised,
        // rather than inventing one here.  The CDU-541 recognises exactly three
        // UNIT ATTENTION codes -- 0x28 caddy inserted, 0x29 power-on/reset,
        // 0x2A mode parameters changed -- and which one applies is known only
        // at the point the condition arises.  Hardcoding 0x28 told a guest that
        // had just ejected a disc that a disc had arrived.
        uint8_t asc = ASC_NOT_READY_TO_READY, ascq = 0x00;
        if (scsi->devices[target].sense.key == SENSE_UNIT_ATTENTION) {
            asc = scsi->devices[target].sense.asc;
            ascq = scsi->devices[target].sense.ascq;
        }
        scsi_check_condition(scsi, SENSE_UNIT_ATTENTION, asc, ascq);
        return;
    }

    // An empty CD bay is a real device on the bus with no disc in it: fail
    // every command that needs the medium, before any of them reach for the
    // absent image.  A machine with a CD bay carries the drive from power-on
    // (system_create), so this is the ordinary state between discs, not an
    // error path.
    //
    // START UNIT joins them, and only in its START form (CDB byte 4 bit 0 —
    // stopping or ejecting an empty drive is fine): a drive with no disc
    // cannot spin one up, so it answers NOT READY.  The Network Server's
    // Open Firmware depends on exactly that.  Its diag-device list is
    // `cd disk6 fd:diags`, so it starts the CD first; told GOOD, it believes
    // the drive came ready and issues READ, and READ's CHECK CONDITION
    // reaches its SCRIPTS program as a DATA IN phase mismatch it never
    // recovers from — the machine stalls at `cd` and never reaches the
    // diagnostic floppy.  Told NOT READY, it reads the sense, gives up on
    // the empty drive and boots the floppy, which is what the hardware does.
    if (scsi->devices[target].type == scsi_dev_cdrom && !scsi->devices[target].medium_present) {
        bool start_unit = scsi->cmd.opcode == CMD_START_STOP_UNIT && (scsi->buf.data[4] & 0x01) != 0;
        if (start_unit || scsi_cmd_needs_medium(scsi->cmd.opcode)) {
            scsi_check_condition(scsi, SENSE_NOT_READY, ASC_SONY_CADDY_NOT_INSERTED, 0x00);
            return;
        }
    }

    switch (scsi->cmd.opcode) {

    case CMD_TEST_UNIT_READY:
        LOG(1, "command: TEST UNIT READY");
        // Check if medium is present for CD-ROM
        if (scsi->devices[target].type == scsi_dev_cdrom && !scsi->devices[target].medium_present) {
            scsi_check_condition(scsi, SENSE_NOT_READY, ASC_SONY_CADDY_NOT_INSERTED, 0x00);
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
        // 6-byte CDB LBA: 5 low bits of data[1] form bits [20:16], data[2..3]
        // form bits [15:0]. Promote each byte to uint32_t before shifting so
        // the result is unambiguous regardless of int width.
        scsi->cmd.lba = (((uint32_t)scsi->buf.data[1] & 0x1F) << 16) | ((uint32_t)scsi->buf.data[2] << 8) |
                        (uint32_t)scsi->buf.data[3];
        // 6-byte CDB tl: 0 means 256 blocks (per SCSI-1). Compute (tl - 1) mod
        // 256 + 1 to convert; mask after the subtract to keep it inside uint8.
        scsi->cmd.tl = (uint16_t)((((uint16_t)scsi->buf.data[4] - 1) & 0xFF) + 1);

        // FLAG/LINK and non-zero LUN aren't modeled. Decline with CHECK
        // CONDITION rather than asserting — the guest may legitimately try.
        if ((scsi->buf.data[5] & 0x3) != 0 || scsi->cmd.lun != 0) {
            LOG(1, "SCSI %s with FLAG/LINK or LUN!=0 (data[5]=0x%02X lun=%u) — declining",
                scsi->cmd.opcode == CMD_WRITE ? "WRITE" : "READ", scsi->buf.data[5], scsi->cmd.lun);
            scsi_check_condition(scsi, SENSE_ILLEGAL_REQUEST, ASC_INVALID_OPCODE, 0x00);
            break;
        }

        uint16_t blk_sz = scsi->devices[target].block_size;

        LOG(1, "SCSI %s target=%d lba=%u tl=%u blk_sz=%u raw_size=%zu",
            scsi->cmd.opcode == CMD_WRITE ? "WRITE" : "READ", target, scsi->cmd.lba, scsi->cmd.tl, blk_sz,
            scsi->device_images[target] ? scsi->device_images[target]->raw_size : 0);

        if (getenv("GS_IIFX_SHIM_TRACE"))
            fprintf(stdout, "SCSI_%s_6 tgt=%d lba=%u tl=%u blk_sz=%u\n", scsi->cmd.opcode == CMD_WRITE ? "WR" : "RD",
                    target, scsi->cmd.lba, scsi->cmd.tl, blk_sz);

        // Reject writes on read-only devices (CD-ROM, etc.)
        if (scsi->cmd.opcode == CMD_WRITE && scsi->devices[target].read_only) {
            scsi_check_condition(scsi, SENSE_DATA_PROTECT, ASC_WRITE_PROTECTED, 0x00);
            break;
        }

        // A 6-byte CDB's transfer length is 8 bits (0 meaning 256), so the
        // product tops out at 256 blocks — 128 KB at 512-byte blocks, but
        // 512 KB on a CD-ROM's 2048-byte blocks.  BUF_LIMIT is 256 * 512, so
        // this used to reject any CD-ROM READ(6) of more than 64 blocks with
        // ILLEGAL REQUEST.  The comment here used to claim "the product stays
        // inside BUF_LIMIT in practice", which is only true for 512-byte
        // blocks and is why no hard-disk row ever caught it: booting System
        // 7.5.3 off a CD dies on `lba=2531 tl=124` (248 KB), and the System
        // reports the refused read as "Not enough memory is available".
        //
        // Grow the staging buffer instead, exactly as the 10-byte path below
        // already does (see scsi_buf_ensure in phase_data_in/out).  Promote
        // both operands before the multiply so an overflow can't slip past a
        // 32-bit-int host.
        // Validate the range BEFORE any phase change, for READ and WRITE alike.
        // phase_data_in/out call scsi_buf_ensure, which reallocs the staging
        // buffer to the full requested size -- and the buffer never shrinks.
        // With the check second, a READ(10) of tl=0xFFFF against a 32 KB
        // medium was correctly refused with CHECK CONDITION and STILL left a
        // 32 MB staging buffer behind for the life of the machine (134 MB at a
        // CD-ROM's 2048-byte blocks), from one CDB the code already knew was
        // invalid.  And if that realloc fails, GS_ASSERTF continues (and is
        // compiled out under GS_FAST), so buf.data becomes NULL and the copy
        // below writes through it.  Nothing may grow the buffer until the
        // request is known to be sane.
        //
        // For WRITE there is a second reason: refusing after the initiator has
        // already pushed a whole data-out phase is a late phase change some
        // initiators do not recover from (the Network Server's SCRIPTS program
        // is one -- see the empty-CD-bay note above).
        size_t byte_off = 0, byte_cnt = 0;
        if (!scsi_lba_range_ok(scsi, target, &byte_off, &byte_cnt)) {
            LOG(1, "SCSI %s out of range: target=%d lba=%u tl=%u blk_sz=%u raw_size=%zu",
                scsi->cmd.opcode == CMD_WRITE ? "WRITE" : "READ", target, scsi->cmd.lba, scsi->cmd.tl, blk_sz,
                disk_size(scsi->device_images[target]));
            scsi_check_condition(scsi, SENSE_ILLEGAL_REQUEST, ASC_LBA_OUT_OF_RANGE, 0x00);
            break;
        }

        // Past the check, the size is known to fit the medium.  Narrate the
        // legitimately large ones: the Apple SCSI driver does issue these.
        if (byte_cnt > BUF_LIMIT)
            LOG(2, "SCSI %s large transfer: tl=%u blk_sz=%u (%zu bytes > BUF_LIMIT)",
                scsi->cmd.opcode == CMD_WRITE ? "WRITE" : "READ", scsi->cmd.tl, blk_sz, byte_cnt);

        if (scsi->cmd.opcode == CMD_WRITE) {
            phase_data_out(scsi, (int)byte_cnt);
        } else {
            phase_data_in(scsi, (int)byte_cnt);
            size_t n = disk_read_data(scsi->device_images[target], byte_off, scsi->buf.data, byte_cnt);
            assert(n == byte_cnt);
            // TEMP DIAG (GS_DEVDIR): dump what storage delivered for the /dev
            // directory read (abs LBA 68698). Valid head: 00 00 3C 00 00 0C 00 01 2E.
            if (getenv("GS_DEVDIR") && scsi->cmd.lba == 68698) {
                fprintf(stderr, "[DEVDIR-STORAGE] lba=68698 n=%zu buf[0:24]=", n);
                for (int i = 0; i < 24; i++)
                    fprintf(stderr, "%02x ", scsi->buf.data[i]);
                fprintf(stderr, "\n");
            }
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

        LOG(1, "SCSI %s(10) target=%d lba=%u tl=%u blk_sz=%u", scsi->cmd.opcode == CMD_WRITE_10 ? "WRITE" : "READ",
            target, scsi->cmd.lba, scsi->cmd.tl, blk_sz);

        if (getenv("GS_IIFX_SHIM_TRACE"))
            fprintf(stdout, "SCSI_%s_10 tgt=%d lba=%u tl=%u blk_sz=%u\n",
                    scsi->cmd.opcode == CMD_WRITE_10 ? "WR" : "RD", target, scsi->cmd.lba, scsi->cmd.tl, blk_sz);

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

        // A 10-byte CDB carries a 16-bit transfer length, so a single command
        // can move far more than the 256-block (BUF_LIMIT) staging buffer holds
        // — the Apple SCSI driver does exactly this writing the System file
        // during a System 7.1 install.  The staging buffer grows to fit (see
        // scsi_buf_ensure in phase_data_in/out); no cap, no assert.
        // Range check first, then the phase change -- see the 6-byte path
        // above for why the order matters.  The largest product that can
        // reach here is 65535 blocks x 2048 bytes = 134,215,680, which fits
        // the int that phase_data_in/out take; a block size of 32 KB or more
        // would overflow it, so cast from the checked size_t rather than
        // recomputing the multiply in int.
        size_t byte_off = 0, byte_cnt = 0;
        if (!scsi_lba_range_ok(scsi, target, &byte_off, &byte_cnt)) {
            LOG(1, "SCSI %s_10 out of range: target=%d lba=%u tl=%u blk_sz=%u raw_size=%zu",
                scsi->cmd.opcode == CMD_WRITE_10 ? "WRITE" : "READ", target, scsi->cmd.lba, scsi->cmd.tl, blk_sz,
                disk_size(scsi->device_images[target]));
            scsi_check_condition(scsi, SENSE_ILLEGAL_REQUEST, ASC_LBA_OUT_OF_RANGE, 0x00);
            break;
        }

        if (byte_cnt > BUF_LIMIT)
            LOG(2, "SCSI %s_10 large transfer: tl=%u blk_sz=%u (%zu bytes > BUF_LIMIT)",
                scsi->cmd.opcode == CMD_WRITE_10 ? "WRITE" : "READ", scsi->cmd.tl, blk_sz, byte_cnt);

        if (scsi->cmd.opcode == CMD_WRITE_10) {
            phase_data_out(scsi, (int)byte_cnt);
        } else {
            phase_data_in(scsi, (int)byte_cnt);
            size_t n = disk_read_data(scsi->device_images[target], byte_off, scsi->buf.data, byte_cnt);
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

        // [6]: byte 4 is the "allocation length".  Zero is a legal probe
        // meaning "send nothing" -- ANSI X3.131-1986's INQUIRY section (Table
        // 7-8) is explicit: "An allocation length of zero indicates that no
        // INQUIRY data shall be transferred.  This condition shall not be
        // considered as an error."  This used to substitute 36.
        scsi->cmd.tl = scsi->buf.data[4];
        scsi->cmd.lun = scsi->buf.data[1] >> 5;

        // EVPD: the command is asking for a VITAL PRODUCT DATA page, not
        // the standard inquiry data, and answering with the standard data
        // is not a harmless approximation — the initiator parses the reply
        // as the page it asked for.  SCSI-2 §8.2.5.1: a target that does
        // not support the requested page "shall return CHECK CONDITION
        // status with the sense key set to ILLEGAL REQUEST and an
        // additional sense code of INVALID FIELD IN CDB".
        //
        // Two pages are served: $00, the list of supported pages, and —
        // for hard disks — IBM's vendor page $C7, the "Self-Configuring
        // SCSI Device" contract.  AIX's configuration methods classify an
        // otherwise-unknown drive by asking for page $C7 and checking for
        // the keyword "SCDD" (`sccheck.c`, AIX 4.1.3); a drive that
        // answers is configured entirely from the page — capacity, queue
        // depth, reset delay, command timeouts — where one that does not
        // is an "Other SCSI Disk" whose size the BOS install reads as
        // zero and refuses to install to.
        if (scsi->buf.data[1] & 0x01u) {
            uint8_t page = scsi->buf.data[2];
            bool is_disk = scsi->devices[target].type != scsi_dev_cdrom;
            if (page == 0x00u) {
                // Page $00: the supported-pages list — itself, plus $C7 on disks.
                uint8_t pg0[6] = {0, 0, 0, 2, 0x00u, 0xC7u};
                pg0[0] = (uint8_t)(is_disk ? 0x00u : 0x05u); // device type
                if (!is_disk)
                    pg0[3] = 1; // CD-ROMs list only page $00
                int n = scsi_data_in_alloc(scsi, 4 + pg0[3], scsi->cmd.tl);
                LOG(2, "INQUIRY target=%d EVPD page $00 -> %d bytes", target, n);
                if (n > 0)
                    memcpy(scsi->buf.data, pg0, (size_t)n);
                break;
            }
            if (page == 0xC7u && is_disk) {
                // The disk SCSD page, byte-for-byte per the chart in IBM's
                // `cfghscsi.h` (struct disk_scsd_inqry_data): 4-byte page
                // header + 113 bytes of self-description.
                image_t *image = scsi->device_images[target];
                uint32_t cap_mb = image ? (uint32_t)(disk_size(image) / (1024u * 1024u)) : 0u;
                uint8_t pg[117];
                memset(pg, 0, sizeof(pg));
                pg[1] = 0xC7u; // page code
                pg[3] = 113u; // page length
                pg[4] = 4u; // SCDD id length
                memcpy(&pg[5], "SCDD", 4); // the keyword the classifier checks
                pg[9] = 1u; // one LUN
                pg[10] = (uint8_t)(cap_mb >> 24); // capacity in MB, big-endian
                pg[11] = (uint8_t)(cap_mb >> 16);
                pg[12] = (uint8_t)(cap_mb >> 8);
                pg[13] = (uint8_t)cap_mb;
                pg[20] = 1u; // queue depth 1: this model queues nothing
                pg[23] = 100u; // ready 100 ms after a bus-device reset
                pg[33] = 0x02u; // technology: supported SCSI disk
                pg[34] = 0x01u; // interface: single-ended
                pg[36] = 30u; // read/write timeout, seconds
                pg[38] = 120u; // write buffer
                pg[40] = 120u; // read buffer
                pg[42] = 120u; // send diagnostics
                pg[43] = (uint8_t)(600u >> 8); // format unit
                pg[44] = (uint8_t)600u;
                pg[46] = 60u; // start unit
                pg[48] = 120u; // reassign block
                pg[59] = 3u; // OS identifier length...
                memcpy(&pg[60], "AIX", 3); // ...and the identifier itself
                pg[72] = 3u; // max retry count
                int n = scsi_data_in_alloc(scsi, (int)sizeof(pg), scsi->cmd.tl);
                LOG(2, "INQUIRY target=%d EVPD page $C7 -> %d bytes (%u MB)", target, n, cap_mb);
                if (n > 0)
                    memcpy(scsi->buf.data, pg, (size_t)n);
                break;
            }
            LOG(2, "INQUIRY target=%d EVPD page $%02X unsupported", target, page);
            scsi_check_condition(scsi, SENSE_ILLEGAL_REQUEST, ASC_INVALID_FIELD_IN_CDB, 0x00);
            break;
        }

        // The allocation length is a CEILING, not a request: "the target
        // shall terminate the DATA IN phase when [it] has transferred all
        // available data" (SCSI-2 §7.5.3).  The standard response this
        // model builds is 36 bytes, so a driver that offers more gets 36
        // and a residual — which is what a real drive gives it, and what
        // the additional-length byte below has to agree with.  (The EVPD
        // pages above have their own lengths; this cap is the standard
        // response's only.)
        scsi->cmd.tl = scsi_data_in_alloc(scsi, 36, scsi->cmd.tl);
        if (scsi->cmd.tl == 0)
            break; // zero allocation: already in STATUS, nothing to fill

        memset(scsi->buf.data, 0, scsi->cmd.tl);

        // A LUN this target does not implement must still ANSWER — SCSI-2
        // §8.2.5: "If the target is not capable of supporting a device on
        // the specified logical unit, the target shall return the INQUIRY
        // data with the peripheral qualifier set to the value required in
        // 7.3.2" — qualifier 011b, device type 1Fh, so byte 0 reads $7F —
        // "with a GOOD status".  Returning the LUN-0 device instead makes
        // every target look like eight identical drives, which is what a
        // prober that walks LUNs sees: the Apple Network Server's Open
        // Firmware `probe-scsi1` listed `Unit 0` through `Unit 7` as the
        // same disk before this was here, and AIX would have configured
        // all eight.  Every device this emulator models is single-LUN.
        if (scsi->cmd.lun != 0) {
            scsi->buf.data[0] = 0x7Fu; // qualifier 011b + device type 1Fh
            if (scsi->cmd.tl >= 5)
                scsi->buf.data[4] = 0x1Fu; // additional length, as for a real one
            LOG(2, "INQUIRY target=%d lun=%u -> not present ($7F)", target, scsi->cmd.lun);
            break;
        }

        // The standard INQUIRY response.  A CD-ROM and a hard disk differ in
        // exactly two bytes -- the peripheral device type, and whether the
        // medium is removable -- and agreed on the other thirty-four, which is
        // why this was two twenty-six-line branches that had to be kept in
        // step by hand.
        {
            bool is_cdrom = scsi->devices[target].type == scsi_dev_cdrom;
            scsi->buf.data[0] = is_cdrom ? 0x05u : 0x00u; // CD-ROM / direct-access
            scsi->buf.data[1] = is_cdrom ? 0x80u : 0x00u; // RMB: removable medium
            scsi->buf.data[2] = 0x01; // SCSI-1 (ANSI version)
            scsi->buf.data[3] = 0x01; // response data format: CCS
            scsi->buf.data[4] = 0x1F; // additional length: 31 -> 36 total
            // Bytes 5-7: zero
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

    case CMD_SEND_DIAGNOSTIC: {
        // SEND DIAGNOSTIC (SCSI-2 §8.2.15): the self-test of an emulated
        // drive always passes.  Bytes 3-4 are the parameter list length; a
        // non-zero list is accepted and discarded.  AIX's BOS install runs
        // this against every target disk as its "preliminary diagnostic
        // test" and refuses to install to a drive that fails it.
        int param_len = (scsi->buf.data[3] << 8) | scsi->buf.data[4];
        LOG(1, "command: SEND DIAGNOSTIC (len=%d)", param_len);
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
            // HD MODE SENSE(6): CDB byte 2 bits 5:0 = page code, byte 4 =
            // allocation length.  Build the full response in a scratch buffer,
            // then transfer only as much as the initiator asked for — SCSI-2
            // §8.2.10 requires the target to send the lesser of the two, and
            // Apple's formatters do issue short allocation lengths when they
            // only want the header.
            uint8_t page_code = scsi->buf.data[2] & 0x3F;
            int alloc_len = scsi->buf.data[4];
            uint32_t blocks = (uint32_t)(disk_size(scsi->device_images[target]) / 512);

            // Vendor-specific page 0x30 carries Apple's drive-identification
            // string.  This — not the INQUIRY vendor/product ID — is what
            // Apple HD SC Setup and Drive Setup gate on when deciding whether
            // a mechanism is an Apple-shipped drive they are willing to format.
            static const char apple_id[] = "APPLE COMPUTER, INC.";
            const int apple_id_len = (int)sizeof(apple_id) - 1; // no NUL on the wire

            uint8_t resp[96];
            memset(resp, 0, sizeof(resp));
            int total = 4 + 8; // mode parameter header + one block descriptor
            resp[3] = 8; // block descriptor length
            // Block descriptor: number of blocks, then 512-byte block length
            resp[5] = (blocks >> 16) & 0xFF;
            resp[6] = (blocks >> 8) & 0xFF;
            resp[7] = blocks & 0xFF;
            resp[9] = 0x00;
            resp[10] = 0x02;
            resp[11] = 0x00;

            // A CHS geometry for the physical-layout pages.  READ CAPACITY
            // stays authoritative for the addressable block count — as on a
            // real drive, cylinders*heads*sectors is the platter geometry and
            // lands at or just under it — so 63x16 is divided out and the
            // cylinder count truncates.
            uint32_t secs_per_track = 63, heads = 16;
            uint32_t cylinders = blocks / (secs_per_track * heads);
            if (cylinders == 0) { // a mechanism smaller than one cylinder
                cylinders = 1;
                heads = 1;
                secs_per_track = blocks > 0xFFFFu ? 0xFFFFu : blocks;
            }

            // Page 3 (Format Device) and page 4 (Rigid Disk Drive Geometry),
            // SCSI-2 §8.3.3/§8.3.4.  Without them a host that asks for the
            // physical parameters is handed a GOOD status and an empty page
            // area, and reads whatever its own buffer held — MkLinux DR3's rz
            // driver does exactly that and derives a sector size from the
            // leftovers of its previous INQUIRY (measured: 14384 bytes/sector
            // off "DPES-31080"), which makes every later block access wrong.
            if (page_code == 0x03 || page_code == 0x3F) {
                resp[total] = 0x03; // page code
                resp[total + 1] = 0x16; // page length: 22 bytes follow
                resp[total + 10] = (secs_per_track >> 8) & 0xFF; // sectors per track
                resp[total + 11] = secs_per_track & 0xFF;
                resp[total + 12] = 0x02; // bytes per physical sector = 512
                resp[total + 13] = 0x00;
                resp[total + 15] = 0x01; // interleave 1:1
                resp[total + 20] = 0x40; // HSEC: hard-sectored, the usual for a fixed disk
                total += 24;
            }
            if (page_code == 0x04 || page_code == 0x3F) {
                resp[total] = 0x04; // page code
                resp[total + 1] = 0x16; // page length: 22 bytes follow
                resp[total + 2] = (cylinders >> 16) & 0xFF; // number of cylinders
                resp[total + 3] = (cylinders >> 8) & 0xFF;
                resp[total + 4] = cylinders & 0xFF;
                resp[total + 5] = (uint8_t)heads; // number of heads
                resp[total + 20] = 0x15; // medium rotation rate: 5400 rpm
                resp[total + 21] = 0x18;
                total += 24;
            }
            if (page_code == 0x30 || page_code == 0x3F) {
                // Page control 0 ("current values") unconditionally: this path
                // masks the CDB with 0x3F above and so never sees bits 7:6, so
                // it has never distinguished current / changeable / default /
                // saved.  Preserved as-is here rather than quietly changed --
                // it is a gap in the HD MODE SENSE, not part of this change.
                total +=
                    scsi_build_apple_page_30(resp + total, /*page_control=*/0, apple_id, apple_id_len, apple_id_len);
            }
            resp[0] = (uint8_t)(total - 1); // mode data length excludes itself

            // Allocation length 0 is legal and means "no data".  This path
            // always had it right; it now shares the helper with everything
            // else that carries an allocation length.
            int n = scsi_data_in_alloc(scsi, total, alloc_len);
            if (n > 0)
                memcpy(scsi->buf.data, resp, (size_t)n);
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
        image_t *image = scsi->device_images[target];
        uint16_t blk_sz = scsi->devices[target].block_size;
        size_t sz = disk_size(image) / blk_sz;

        phase_data_in(scsi, 8);
        // memcpy through uint8_t* to dodge strict-aliasing UB; gcc/clang fold
        // constant-size memcpy to a single MOV.
        uint32_t last_lba = BE32((uint32_t)sz - 1);
        uint32_t be_blk_sz = BE32((uint32_t)blk_sz);
        memcpy(scsi->buf.data, &last_lba, 4);
        memcpy(scsi->buf.data + 4, &be_blk_sz, 4);

        break;
    }

    case CMD_VERIFY:
        // Verify data on disc — no-op (always succeeds)
        LOG(1, "command: VERIFY");
        phase_status(scsi, STATUS_GOOD);
        break;

    case CMD_RESERVE:
    case CMD_RELEASE:
        // RESERVE/RELEASE (SCSI-2 §9.2.10-11): with a single initiator the
        // reservation can never conflict, so both always succeed.  AIX's
        // scdisk reserves the disk in its open path and fails the open —
        // errno EINVAL, an unusable hdisk — when the reservation does not
        // take.
        LOG(1, "command: %s", scsi->cmd.opcode == CMD_RESERVE ? "RESERVE" : "RELEASE");
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

    // Sony vendor READ TOC (C1h): the AppleCD SC's Sony CDU-8002 vendor command,
    // used exclusively by the early Apple CD-ROM driver (System 7.x).  Its TOC
    // Data Format (Sony CDU-541 manual §5.2.23, Table 5-23) uses 6-byte track
    // descriptors, NOT the SCSI-2 (43h) 8-byte layout — answering with the 43h
    // format makes the driver mis-read a data disc as an audio CD.
    case CMD_SONY_READ_TOC:
        if (scsi->devices[target].type == scsi_dev_cdrom)
            scsi_cdrom_read_toc_sony(scsi);
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
void command_complete(scsi_t *scsi) {
    int target = scsi->bus.target & 7;
    uint16_t blk_sz = scsi->devices[target].block_size;

    switch (scsi->cmd.opcode) {

    case CMD_WRITE:
    case CMD_WRITE_10: {
        // run_cmd already refused an out-of-range range at CDB decode; this is
        // the check for a data phase that did not deliver what was asked for.
        // Both used to be assert()s, which -DNDEBUG removes from the release
        // wasm build -- so the failure they were meant to catch became a silent
        // short write reported as STATUS GOOD (03-scsi F-02).
        size_t byte_off = 0, byte_cnt = 0;
        if (!scsi_lba_range_ok(scsi, target, &byte_off, &byte_cnt) || byte_cnt != scsi->buf.size) {
            LOG(1, "SCSI WRITE: refusing tl=%u blk_sz=%u (%zu bytes) against buf.size=%zu raw_size=%zu", scsi->cmd.tl,
                blk_sz, byte_cnt, scsi->buf.size, disk_size(scsi->device_images[target]));
            scsi->buf.max = scsi->buf.size = 0;
            scsi_check_condition(scsi, SENSE_ILLEGAL_REQUEST, ASC_LBA_OUT_OF_RANGE, 0x00);
            return;
        }

        // And report a short write rather than discarding the count: an
        // in-bounds backing-store failure is a MEDIUM ERROR, not success.
        size_t wrote = disk_write_data(scsi->device_images[target], byte_off, scsi->buf.data, byte_cnt);
        scsi->buf.max = scsi->buf.size = 0;
        if (wrote != byte_cnt) {
            LOG(1, "SCSI WRITE: storage took %zu of %zu bytes at offset %zu", wrote, byte_cnt, byte_off);
            scsi_check_condition(scsi, SENSE_MEDIUM_ERROR, ASC_WRITE_FAULT, 0x00);
            return;
        }
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

// Add a SCSI device to the bus at the specified SCSI ID

// ============================================================================
// Selection time-out
// ============================================================================

static void scsi_bus_seltmo_event(void *source, uint64_t data) {
    (void)data;
    scsi_t *bus = (scsi_t *)source;
    scsi_select_timeout_fn fn = bus->seltmo_fn;
    void *ctx = bus->seltmo_ctx;
    bus->seltmo_fn = NULL;
    bus->seltmo_ctx = NULL;
    if (fn)
        fn(ctx);
}

void scsi_bus_arm_select_timeout(scsi_t *bus, uint64_t ns, scsi_select_timeout_fn fn, void *ctx) {
    if (!bus || !fn)
        return;
    scheduler_t *s = system_scheduler();
    if (!s) {
        // No scheduler underneath: the unit suites drive the models directly,
        // and there is no time for a wait to pass in.
        fn(ctx);
        return;
    }
    if (!bus->seltmo_registered) {
        scheduler_new_event_type(s, "scsi", bus, "select_timeout", &scsi_bus_seltmo_event);
        bus->seltmo_registered = true;
    }
    bus->seltmo_fn = fn;
    bus->seltmo_ctx = ctx;
    remove_event(s, &scsi_bus_seltmo_event, bus);
    if (ns != 0) {
        scheduler_new_cpu_event(s, &scsi_bus_seltmo_event, bus, 0, 0, ns);
        return;
    }
    // A zero period: the driver selected before programming its time-out
    // register, which MkLinux DR3's 53c94 driver does on every empty ID of its
    // bus scan.  ONE CYCLE, not a substituted default -- a zero-delay insert
    // already landed on the current timestamp and fired at the next queue
    // drain, so one cycle is that same behaviour spelled legally.  Handing it
    // ANSI's 250 ms instead would invent a wait the guest never asked for and
    // stretch every empty ID of that scan.
    //
    // What it must NOT do is call back synchronously: that would complete the
    // select-fail-report-retry cycle inside the driver's own register write,
    // which is the whole thing this helper exists to prevent.
    scheduler_new_cpu_event(s, &scsi_bus_seltmo_event, bus, 0, 1, 0);
}

void scsi_bus_cancel_select_timeout(scsi_t *bus) {
    if (!bus)
        return;
    bus->seltmo_fn = NULL;
    bus->seltmo_ctx = NULL;
    scheduler_t *s = system_scheduler();
    if (s)
        remove_event(s, &scsi_bus_seltmo_event, bus);
}

// A SCSI bus reset, as every device on the wire sees it.
//
// RST/ is one signal.  Nothing in the NCR 5380 design manual, the NCR
// 53C94/95/96 data manual, the LSI53C825A technical manual or ANSI X3.131-1986
// suggests a target behaves differently according to which initiator asserted
// it, so this is deliberately ONE implementation that all four controllers
// call.  Before it existed there were four partial ones, and the only bus-level
// entry point on offer was scsi_reset_pin(), which resets a 5380 register file
// -- so a Power Macintosh with no 5380 anywhere called it, and the front-ends
// that declined hand-rolled their own.
//
// ANSI X3.131-1986 S5.2.2.1, the "hard" RESET option, says devices shall:
//   (1) Clear all uncompleted commands
//   (2) Release all SCSI device reservations
//   (3) Return any SCSI device operating modes (MODE SELECT, PREVENT/ALLOW
//       MEDIUM REMOVAL commands, etc) to their default conditions.
//
// and S6.1.3 adds the unit attention: one "shall begin for each initiator
// whenever the removable medium may have been changed or the target has been
// reset (by a BUS DEVICE RESET message or a 'hard' RESET condition)".  The
// CDU-541 manual supplies the code its UNIT ATTENTION table uses for that case,
// 0x29 "Power on, reset or BUS DEVICE RESET occurred", and independently
// confirms the prevent bit clears "by the receipt of a BUS DEVICE RESET message
// from any initiator or by a reset condition" (S5.2.14).
//
// Reservations (2) need nothing: RESERVE and RELEASE are modelled as always
// succeeding because a single initiator can never conflict, so there is no
// reservation state to release.  That is an absence, not an omission.
void scsi_bus_reset(scsi_t *bus) {
    if (!bus)
        return;

    // (1) Whatever was in flight is abandoned, and the wire goes free.
    scsi_bus_cancel_select_timeout(bus);
    phase_free(bus);
    bus->buf.size = bus->buf.max = 0;
    bus->buf.pos = 0;

    for (int i = 0; i < 8; i++) {
        if (bus->devices[i].type == scsi_dev_none)
            continue;

        // (3) Operating modes back to their defaults.
        bus->devices[i].prevent_removal = false;
        if (bus->devices[i].default_block_size)
            bus->devices[i].block_size = bus->devices[i].default_block_size;

        // S6.1.3: the reset itself raises a unit attention on every target,
        // reported to the first command that is not exempt.
        bus->devices[i].unit_attention = true;
        scsi_set_sense(bus, i, SENSE_UNIT_ATTENTION, ASC_POWER_ON_OR_RESET, 0x00);
    }
    LOG(2, "bus reset: devices returned to their power-on state");
}

// Emit Apple's vendor-identification MODE SENSE page $30.
//
// Apple never published this page; what is known of it comes from the drivers
// that read it.  It is the mechanism that is shared here, NOT the content --
// the hard-disk and CD-ROM identification strings differ, and the evidence for
// each is different in kind:
//
//   HD  "APPLE COMPUTER, INC." -- 20 bytes, trailing period.  This one is
//       load-bearing and verified: HD SC Setup requests it four times during
//       se30-format-hd, and it is what the formatter gates on when deciding
//       whether a mechanism is an Apple-shipped drive.
//
//   CD  "APPLE COMPUTER, INC   " -- 22 bytes, no period, three trailing
//       spaces, padded to a 30-byte page.  UNVERIFIED: no test in this tree
//       requests it.  Instrumenting build_page_30 across se30-cdrom and
//       iici-cdrom-boot counted ZERO calls, which matches the CDU-8002 being a
//       1991 SCSI-1 drive while page $30 arrived with System 7.5+ drivers.
//
// So the two are NOT known to be the same string, and are deliberately not
// forced to be.  Making them agree would mean changing the untested side to
// match the tested one on an assumption nothing here can check.  If a later
// image does exercise the CD-ROM gate, that is the moment to find out.
//
// `id_len` is the page length: the payload that follows the two-byte header.
// Bytes beyond the string are left zero, which is the padding both forms use.
int scsi_build_apple_page_30(uint8_t *buf, int page_control, const char *id, int id_len, int page_len) {
    buf[0] = 0x30; // page code
    buf[1] = (uint8_t)page_len;
    memset(buf + 2, 0, (size_t)page_len);
    // Page control 1 is "changeable values", and nothing here is changeable,
    // so the mask stays all-zero.
    if (page_control != 1)
        memcpy(buf + 2, id, (size_t)id_len);
    return 2 + page_len;
}

void scsi_add_device(scsi_t *restrict scsi, int scsi_id, const char *vendor, const char *product, const char *revision,
                     image_t *image, enum scsi_device_type type, uint16_t block_size, bool read_only) {
    // scsi_id 7 is reserved for the Mac initiator; only targets 0..6 are valid.
    GS_ASSERTF(scsi_id < 7, "scsi_add_device: scsi_id %d is the initiator slot, expected 0..6", scsi_id);

    scsi->device_images[scsi_id] = image;
    scsi->devices[scsi_id].type = type;
    scsi->devices[scsi_id].block_size = block_size;
    scsi->devices[scsi_id].default_block_size = block_size;
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

    // Inserting a caddy and recovering its TOC is the media-change cause the
    // CDU-541 manual 4.1.3 names, and 0x28 "Not ready to ready transition
    // (caddy inserted)" is the code it reports.  Stage it here, where the cause
    // is known, so the UNIT ATTENTION gate in run_cmd can simply report what is
    // pending instead of guessing.
    if (type == scsi_dev_cdrom && image != NULL) {
        scsi->devices[scsi_id].unit_attention = true;
        scsi_set_sense(scsi, scsi_id, SENSE_UNIT_ATTENTION, ASC_NOT_READY_TO_READY, 0x00);
    }
}

bool scsi_pop_data_in_byte(scsi_t *scsi, uint8_t *out) {
    // Mirrors the chip's CDR-read code path inside read_uint8() — same
    // sequence of side effects, so an external DMA pump produces the
    // same chip-side state as a CPU-driven byte loop would have.
    if (!scsi || !out)
        return false;
    if (scsi->bus.phase != scsi_data_in)
        return false;
    if (scsi->buf.size == 0)
        return false;
    *out = next_byte(scsi); // decrements buf.size
    // REQ/ACK handshake (MR_DMA pop path only).  A real SCSI target in
    // DATA IN asserts /REQ whenever it has another byte ready for the
    // initiator: after each byte is ACK'd /REQ drops briefly and then
    // re-asserts for the next byte, and only stays low for good once the
    // target's data is exhausted (it then switches to STATUS).  We move
    // bytes synchronously, so the only externally-visible REQ state is the
    // *settled* one observed between DMA bursts — model that: /REQ stays
    // asserted while data remains (buf.size>0), and clears when the target
    // has nothing left.
    //
    // This is load-bearing for the IIfx bus-master scatter-gather READ:
    // after one SG segment's end-of-DMA, the driver (scsitask, poll at
    // $1004B2F4) waits for the target to still be REQ'ing before it reads
    // the SCSI phase and re-arms the next SG segment.  If /REQ were left
    // deasserted (the old unconditional clear), that poll times out, the
    // driver aborts the transfer with only the first segment delivered,
    // and the rest of a multi-page read is left as stale RAM — the source
    // of the residual "bad block" garbage on large (>1 segment) reads.
    if (scsi_5380_dma_mode(scsi)) {
        if (scsi->buf.size > 0)
            scsi->bus.req = true;
        else
            scsi->bus.req = false;
    }
    // Eagerly transition to STATUS when the SCSI command's data has
    // been fully delivered (buf empty).  On real hardware the target
    // releases data-in as soon as it has no more data to hand the
    // initiator.  This matters specifically for A/UX's scsiirq on
    // the IIfx wrapper: it reads BSR at entry and uses BSR_PM to
    // distinguish "the chip just finished a command cleanly"
    // (PM=0 → SI_PHASE → kernel clears MR_DMA, so the next SCSI
    // READ produces a fresh MR_DMA 0→1 edge that reloads the
    // 343S0064-A internal DMA Address Counter from $100) from
    // "this DMA chunk is mid-transfer" (PM=1 → kernel leaves
    // MR_DMA set → next chunk's $100 write does NOT cause a counter
    // reload → chunks naturally concatenate at advancing physical
    // addresses).  Only fires on the bus-master DMA pop path; the
    // CPU's PIO/pseudo-DMA reads of $060 go through read_uint8
    // case ODR (which calls next_byte directly) and are unaffected.
    if (scsi->buf.size == 0)
        phase_status(scsi, STATUS_GOOD);
    return true;
}

// Take one DATA OUT / COMMAND byte onto the bus.
//
// This is the wire's half: stage the byte, and when the phase's expected count
// is reached, dispatch -- run_cmd for a complete CDB, command_complete for a
// complete payload.  No chip is involved.
void scsi_bus_accept_data_out_byte(scsi_t *scsi, uint8_t value) {
    if (!scsi)
        return;
    assert(scsi->buf.size < scsi->buf.max);
    scsi->buf.data[scsi->buf.size++] = value;

    if (scsi->bus.phase == scsi_command) {
        if (scsi->buf.size == cmd_size(scsi->buf.data[0]))
            run_cmd(scsi);
    } else if (scsi->buf.size == scsi->buf.max) {
        command_complete(scsi);
    }
}

// A bus-master front-end pushing one byte of a DATA OUT transfer.
//
// All four controllers call this.  Until the bus was separated from the 5380 it
// ran unconditionally through that chip's auto-handshake model -- ODR, the
// BLIND priming gate, the "Start DMA Send" arm gate -- which meant a Quadra's
// 53C96, a Power Macintosh's SCRIPTS engine and MESH were all pushing bytes
// through the register semantics of a chip their machines do not contain.  It
// worked only because this function set the arm flag first, so every gate
// happened to pass.
//
// Now the 5380 path is taken only when there is a 5380: the IIfx's SDMA engine
// genuinely needs it (see scsi_5380_dma_push_byte).  Everyone else goes
// straight onto the wire, which is what they were effectively doing anyway.
void scsi_push_data_out_byte(scsi_t *scsi, uint8_t byte) {
    if (!scsi)
        return;
    if (scsi->chip5380)
        scsi_5380_dma_push_byte(scsi, byte);
    else
        scsi_bus_accept_data_out_byte(scsi, byte);
}

bool scsi_external_select(scsi_t *scsi, int target) {
    if (!scsi || target < 0 || target > 7)
        return false;
    if (!scsi_device_present(scsi, (unsigned)target))
        return false; // no device: the front-end times out
    if (scsi->bus.phase != scsi_bus_free)
        phase_free(scsi); // a stuck prior transaction never blocks a select
    scsi->bus.initiator = 7; // Macintosh host ID
    scsi->bus.target = target;
    phase_arbitration(scsi);
    phase_selection(scsi);
    phase_command(scsi);
    return true;
}

void scsi_external_data_in_complete(scsi_t *scsi) {
    if (!scsi || scsi->bus.phase != scsi_data_in || scsi->buf.size != 0)
        return;
    phase_status(scsi, STATUS_GOOD);
}

int scsi_external_status_byte(scsi_t *scsi) {
    if (!scsi || scsi->bus.phase != scsi_status)
        return -1;
    int status = scsi->bus.data;
    phase_message_in(scsi, 0x00); // COMMAND COMPLETE
    return status;
}

int scsi_external_message_byte(scsi_t *scsi) {
    if (!scsi || scsi->bus.phase != scsi_message_in)
        return -1;
    return scsi->bus.data;
}

void scsi_external_release(scsi_t *scsi) {
    if (!scsi)
        return;
    phase_free(scsi);
}

// Eject the medium currently in the SCSI device at `id` (0..6).  Mirrors
// the START/STOP UNIT eject path: clear the medium pointer, set the
// medium-not-present unit attention so the host sees a fresh transition.
// Returns 1 on success, 0 if the slot was already empty, -1 on error.
int scsi_eject_device(scsi_t *scsi, int id) {
    if (!scsi || id < 0 || id > 6)
        return -1;
    if (!scsi->device_images[id] && !scsi->devices[id].medium_present)
        return 0;
    scsi->devices[id].medium_present = false;
    scsi->device_images[id] = NULL;
    // Removal raises NO unit attention.  The CDU-541 manual 4.1.3 lists exactly
    // four causes -- power-on, reset, *insertion* of a caddy with successful TOC
    // recovery, and MODE SELECT from another initiator -- and its UNIT ATTENTION
    // table has no code for removal.  An empty bay is a persistent NOT READY
    // state instead, handled in run_cmd for as long as it lasts.  That lifetime
    // is the point: a UNIT ATTENTION is a one-shot cleared by the first CHECK
    // CONDITION, so a guest that ejected, took one error and retried used to
    // find the second command succeeding against an empty drive.
    return 1;
}

// The three phase lines, as ANSI X3.131-1986 Table 5-1 encodes them.
uint8_t scsi_phase_wire_bits(int phase) {
    switch (phase) {
    case scsi_data_out:
        return 0x0; // -  -  -
    case scsi_data_in:
        return 0x1; // -  -  I/O
    case scsi_command:
        return 0x2; // -  C/D -
    case scsi_status:
        return 0x3; // -  C/D I/O
    case scsi_message_out:
        return 0x6; // MSG C/D -
    case scsi_message_in:
        return 0x7; // MSG C/D I/O
    default:
        return 0x0; // bus free / arbitration / selection assert none of them
    }
}

bool scsi_bus_req(const scsi_t *scsi) {
    return scsi && scsi->bus.req;
}

bool scsi_bus_bsy(const scsi_t *scsi) {
    return scsi && scsi->bus.bsy;
}

int scsi_get_bus_phase(const scsi_t *scsi) {
    return scsi ? (int)scsi->bus.phase : 0;
}

int scsi_get_bus_target(const scsi_t *scsi) {
    return scsi ? scsi->bus.target : -1;
}

int scsi_get_bus_initiator(const scsi_t *scsi) {
    return scsi ? scsi->bus.initiator : -1;
}

uint8_t scsi_get_cmd_opcode(const scsi_t *scsi) {
    return scsi ? scsi->cmd.opcode : 0;
}

int scsi_get_cmd_target(const scsi_t *scsi) {
    return scsi ? (scsi->bus.target & 7) : -1;
}

uint32_t scsi_get_cmd_lba(const scsi_t *scsi) {
    return scsi ? (uint32_t)scsi->cmd.lba : 0;
}

uint16_t scsi_get_cmd_tl(const scsi_t *scsi) {
    return scsi ? scsi->cmd.tl : 0;
}

uint16_t scsi_get_cmd_blk_sz(const scsi_t *scsi) {
    if (!scsi)
        return 0;
    int t = scsi->bus.target & 7;
    return scsi->devices[t].block_size;
}

bool scsi_device_present(const scsi_t *scsi, unsigned which) {
    return scsi_device_type(scsi, which) != scsi_dev_none;
}

bool scsi_device_medium_present(const scsi_t *scsi, unsigned which) {
    if (!scsi || which > 7)
        return false;
    return scsi->devices[which].medium_present;
}

struct image *scsi_device_image(const scsi_t *scsi, unsigned which) {
    if (!scsi || which > 7 || scsi->devices[which].type == scsi_dev_none)
        return NULL;
    if (!scsi->devices[which].medium_present)
        return NULL;
    return scsi->device_images[which];
}
