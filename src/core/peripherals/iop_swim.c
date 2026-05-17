// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// iop_swim.c
// SWIM IOP firmware-equivalent state machine.  Models what the 65C02
// firmware running inside the PIC would do, in C, by replicating the
// firmware's host-visible behaviour.  No 6502 emulation: we observe
// the host-side mailbox protocol and produce the corresponding
// host-visible state changes (interrupt raises, mailbox writes,
// scheduler events).
//
// State organisation
// ------------------
// Most state lives in iop->ram[] because that's literally where the
// firmware keeps it.  iop->ram covers the IOP's 32 KB shared RAM
// view ($0000-$7FFF) so all firmware-equivalent state is automatically
// included in the IOP checkpoint.  Specifically:
//
//   $0006-$0014  zero-page ADB-driver vars  (matches firmware ZP)
//   $0200-$021F  XmtMsg state bytes + PatchReqAddr
//   $0220-$02FF  XmtMsg slot payloads
//   $0300-$031F  RcvMsg state bytes + IOPAliveAddr
//   $0320-$03FF  RcvMsg slot payloads
//   $4E00-$4E9F  message-channel & timer free lists + ADB work area
//   $4FE0-$4FFF  *our model's bookkeeping (firmware-reserved scratch)
//
// Channel assignments
// -------------------
// The IOP manager allocates message slots per logical channel:
//
//   slot 1 — IOP-kernel reply channel (no host driver attaches here;
//            used internally by the firmware for unsolicited
//            notifications via $5125 SendMsgToHost).
//   slot 2 — SWIM/floppy channel.  The host's .Sony driver posts
//            xmtReq* commands (Init, DriveStatus, Read, Write, Eject,
//            ...) here; the firmware replies into the same XmtMsg[2]
//            payload buffer.  RcvMsg[2] carries IOP-initiated disk
//            events (rcvReqDiskInserted, rcvReqDiskEjected,
//            rcvReqDiskStatusChanged) — those are unsolicited
//            notifications posted whenever the firmware's drive poll
//            detects a change.
//   slot 3 — ADB channel.  Host posts Talk/Listen commands; firmware
//            bit-bangs them onto the single-wire ADB bus and replies
//            with the device data (or NoReply when nothing answers).
//   slots 4-7 — reserved / unused on the boot path.
//
// Translation strategy
// --------------------
// The firmware is interrupt-driven with these external trigger points:
//
//   1. RUN-bit 0→1     →  iop_swim_on_run_start  (mirrors $5000 entry)
//   2. Host-kick IRQ   →  iop_swim_on_host_kick  (mirrors $5070)
//   3. Timer expiry    →  swim_timer_expire      (mirrors $5331)
//   4. Main-loop tick  →  swim_main_loop_tick    (mirrors $5029 body)
//   5. ADB cmd complete → swim_adb_response      (the host-visible result
//                          of running the firmware's $5610 bit-bang loop)
//   6. Drive poll tick →  swim_drive_poll_tick   (autonomous detection of
//                          disk-inserted/-ejected events when polling is
//                          enabled by xmtReqStartPolling)
//
// Each trigger executes the corresponding firmware code paths translated
// into C, operating on iop->ram[] for state.
//
// For the boot's ADB device-table scan, every Talk-Reg-3 command times
// out (no devices wired through our ADB transport yet), which produces
// a "NoReply" response that lets the host's ADBMgr advance through all
// 16 addresses and clear fDBInit.  When real ADB devices are added,
// the swim_adb_response path will deliver actual data; no model change
// needed.
//
// For the floppy boot path, the firmware behaviour mirrored here is:
//   - At reset, the SWIM/ADB driver state is initialised (see $53D1
//     InitSwimDriver in the annotated firmware).
//   - On xmtReqInitialize, the firmware writes a 4-byte DriveKinds[]
//     table into XmtMsg[2] (one byte per logical drive) and acks.
//   - On xmtReqStartPolling, the firmware starts its autonomous drive-
//     poll loop.  Each cycle of the loop reads the SWIM front-side
//     status byte to detect drive insertion/ejection.  In our model
//     the poll loop runs on a scheduler timer (swim_drive_poll_tick)
//     and consults floppy_drive_image() to decide whether each drive
//     is occupied; transitions produce rcvReqDiskInserted /
//     rcvReqDiskEjected notifications via RcvMsg[2].
//   - On xmtReqDriveStatus, the firmware reads the SWIM controller
//     status bytes and writes a DriveStatus record back into XmtMsg[2].
//     Our model synthesises the same record from the floppy module's
//     state plus the image's type/writable flags.
//   - On xmtReqRead / xmtReqWrite, the firmware programs the PIC's
//     DMA channels to transfer N×512 bytes between the host's
//     BufferAddr and the SWIM serial-in/-out ports.  Our model
//     performs the transfer synchronously via disk_read_data /
//     disk_write_data on the image, writing the result into host
//     RAM via the global memory_map (memory_write_uint8 et al.).

#include "iop_internal.h"

#include "floppy.h"
#include "image.h"
#include "log.h"
#include "memory.h"
#include "scheduler.h"
#include "system.h"
#include "system_config.h"

#include <string.h>

LOG_USE_CATEGORY_NAME("iop_swim");

// ============================================================================
//  Constants matching the firmware's protocol
// ============================================================================

// Channel assignments (IOP Manager + ADB Manager conventions)
#define SWIM_SLOT 2 // SwimMsgNumber = 2 — slot 2 carries floppy commands
#define ADB_SLOT  3 // ADBMsgNum = 3 — slot 3 carries ADB packets

// Host → IOP request codes carried in XmtMsg[2].buf[0] = SwimIopMsg.ReqKind.
// These match the .Sony driver's xmtReq* constants in the System ROM.
#define SWIM_REQ_INITIALIZE      0x01 // returns DriveKinds[0..3] in reply
#define SWIM_REQ_SHUTDOWN        0x02 // stop driver activity
#define SWIM_REQ_START_POLLING   0x03 // begin autonomous drive poll
#define SWIM_REQ_STOP_POLLING    0x04 // pause autonomous drive poll
#define SWIM_REQ_SET_HFS_TAG     0x05 // stash host RAM addr for MFS tags
#define SWIM_REQ_DRIVE_STATUS    0x06 // returns DriveStatus block in reply
#define SWIM_REQ_EJECT           0x07 // eject media from drive
#define SWIM_REQ_FORMAT          0x08 // format media (destructive)
#define SWIM_REQ_FORMAT_VERIFY   0x09 // verify formatting
#define SWIM_REQ_WRITE           0x0A // write N blocks of 512 bytes
#define SWIM_REQ_READ            0x0B // read  N blocks of 512 bytes
#define SWIM_REQ_READ_VERIFY     0x0C // read + compare (no host buffer touch)
#define SWIM_REQ_CACHE_CONTROL   0x0D // track-cache enable/disable
#define SWIM_REQ_TAG_BUF_CONTROL 0x0E // tag-buffer enable/disable
#define SWIM_REQ_GET_ICON        0x0F // fetch media/drive icon resource
#define SWIM_REQ_DUP_INFO        0x10 // disk-duplicator info
#define SWIM_REQ_GET_RAW_DATA    0x11 // copy-protection raw read

// IOP → Host event codes carried in RcvMsg[2].buf[0].
#define SWIM_EVT_DISK_INSERTED       0x01 // disk just appeared in drive
#define SWIM_EVT_DISK_EJECTED        0x02 // disk just removed from drive
#define SWIM_EVT_DISK_STATUS_CHANGED 0x03 // status changed (write-protect, etc.)

// Drive-kind values returned in DriveKinds[0..3] by xmtReqInitialize.
// These match the .Sony driver's noDriveKind / SSGCRDriveKind / etc.
#define DRIVE_KIND_NONE     0x00 // no drive present at this index
#define DRIVE_KIND_UNKNOWN  0x01 // unspecified
#define DRIVE_KIND_SS_GCR   0x02 // 400 KB GCR single-sided
#define DRIVE_KIND_DS_GCR   0x03 // 800 KB GCR double-sided
#define DRIVE_KIND_DSMFM_HD 0x04 // 1.44 MB SuperDrive (GCR + MFM)
#define DRIVE_KIND_HD20     0x07 // HD-20 fixed disk

// SwimIopMsg field offsets within an XmtMsg / RcvMsg payload.  Layout
// matches the .Sony driver's `SwimIopMsg record` in the System ROM.
#define SIM_REQ_KIND     0 // byte: request / event code
#define SIM_DRIVE_NUMBER 1 // byte: logical drive index (0..MaxDriveNumber)
#define SIM_ERROR_CODE   2 // word: returned MacOS error code (signed)
// Initialize-reply layout (overlaps the AdditionalParam union at offset 4):
#define SIM_DRIVE_KINDS 4 // 4 bytes: one DriveKind per logical drive
// DriveStatus-reply layout (also at offset 4):
#define SIM_TRACK            4 // word: current head track
#define SIM_WRITE_PROTECTED  6 // byte: bit 7 = write-protected
#define SIM_DISK_IN_PLACE    7 // byte: 0 = no disk, 1/2 = disk in place
#define SIM_DRIVE_INSTALLED  8 // byte: 0 = unknown, 1 = installed, $FF = no
#define SIM_SIDES            9 // byte: bit 7 = double-sided
#define SIM_TWO_SIDED_FORMAT 10 // byte: $FF = 2-sided disk, $00 = 1-sided
#define SIM_NEW_INTERFACE    11 // byte: $FF = 800K+ / SuperDrive
#define SIM_DISK_ERRORS      12 // word: running disk error count
// ExtDriveStatus layout follows DriveStatus inside the same buffer:
#define SIM_MFM_DRIVE       14 // byte: $FF = SuperDrive
#define SIM_MFM_DISK        15 // byte: $FF = MFM disk in drive
#define SIM_MFM_FORMAT      16 // byte: $FF = 1440K, $00 = 720K
#define SIM_DISK_CONTROLLER 17 // byte: $FF = SWIM, $00 = IWM
#define SIM_CURRENT_FORMAT  18 // word: current-format bit mask
#define SIM_FORMATS_ALLOWED 20 // word: allowable-format bit mask
// Data-transfer layout (Read / Write / ReadVerify / Format):
#define SIM_BUFFER_ADDR  4 // long: host RAM target/source address
#define SIM_BLOCK_NUMBER 8 // long: starting block number (512-byte blocks)
#define SIM_BLOCK_COUNT  12 // long: number of blocks to transfer
#define SIM_MFS_TAG_DATA 16 // 12 bytes: MFS tag bytes (read/write)

// MacOS error codes used in SIM_ERROR_CODE.
#define MAC_ERR_NO_ERR  0 // noErr
#define MAC_ERR_PARAM   -50 // paramErr
#define MAC_ERR_NS_DRV  -56 // NSDrvErr — drive not in queue
#define MAC_ERR_OFFLINE -65 // offLinErr — drive empty
#define MAC_ERR_IO      -36 // ioErr — generic I/O failure
#define MAC_ERR_W_PR    -44 // wPrErr — write protected

// ADBMsg structure offsets within an XmtMsg / RcvMsg slot-3 payload
#define ADBMSG_FLAGS     0
#define ADBMSG_DATACOUNT 1
#define ADBMSG_ADBCMD    2
#define ADBMSG_ADBDATA   3 // .. ADBMSG_ADBDATA+7

// ADBMsg Flags bits (IOP-based ADB transport)
#define ADBMSG_FLAG_NOREPLY    (1u << 1) // command timed out (no device replied)
#define ADBMSG_FLAG_SRQ        (1u << 2) // service-request pending
#define ADBMSG_FLAG_SETPOLL_EN (1u << 5) // SetPollEnables: update DevMap from data
#define ADBMSG_FLAG_POLL_EN    (1u << 6) // PollEnable: enable auto-polling
#define ADBMSG_FLAG_EXPLICIT   (1u << 7) // host-initiated explicit command

// Our model's bookkeeping in the firmware's reserved scratch area.
// (The firmware never touches $4FE0-$4FFF, so we may stash arbitrary state
// here without disturbing host-visible RAM.)
#define SWIM_MODEL_BASE            0x4FE0
#define SWIM_MODEL_ADB_BUSY        (SWIM_MODEL_BASE + 0) // 1 if ADB cmd in flight
#define SWIM_MODEL_ADB_CMD         (SWIM_MODEL_BASE + 1) // echoed back in reply
#define SWIM_MODEL_ADB_FLAGS       (SWIM_MODEL_BASE + 2) // request Flags saved for reply
#define SWIM_MODEL_POLL_ENABLED    (SWIM_MODEL_BASE + 3) // drive-poll loop active
#define SWIM_MODEL_DRIVE_PRESENT   (SWIM_MODEL_BASE + 4) // bitmap: bit N = drive N has disk last seen
#define SWIM_MODEL_DRIVE_ANNOUNCED (SWIM_MODEL_BASE + 5) // bitmap: bit N = DiskInserted has been delivered for drive N
#define SWIM_MODEL_HFS_TAG_ADDR    (SWIM_MODEL_BASE + 8) // long: host RAM addr for MFS tags

// Number of floppy drives we model (must match floppy.c NUM_DRIVES).
#define SWIM_NUM_DRIVES 2

// Timing constants chosen to mimic firmware behaviour without
// cycle-accurate bit-bang modelling:
//   ADB timeout: the firmware's $5648/$5652 loops produce a ~250 us
//     "no-device" window.  We use 1 ms to be comfortably past any
//     host-side timing assumptions while staying short enough that
//     the 16-address scan completes in well under a second.
//   Main-loop tick: the firmware's $5029 main loop runs continuously
//     at ~6502 clock rate (~1 MHz / loop_cycles ≈ many kHz).  For
//     host-visible behaviour we only need to refresh IOPAliveAddr,
//     which is checked once during boot.  10 ms suffices.
//   Drive-poll tick: the .Sony driver's StartPolling expects the IOP
//     to notice disk insertions within roughly one VBL (16.7 ms) of
//     them occurring.  20 ms is conservative and still inside the
//     OS's "no event" patience window.
#define ADB_TIMEOUT_NS     1000000ULL //  1 ms
#define MAIN_LOOP_TICK_NS  10000000ULL // 10 ms
#define DRIVE_POLL_TICK_NS 20000000ULL // 20 ms

// ============================================================================
//  Forward declarations
// ============================================================================

static void swim_main_loop_tick(void *source, uint64_t data);
static void swim_adb_response(void *source, uint64_t data);
static void swim_drive_poll_tick(void *source, uint64_t data);
static void swim_handle_xmt_slot(iop_t *iop, int slot);
static void swim_handle_rcv_drain(iop_t *iop, int slot);
static void swim_start_adb_send(iop_t *iop, uint8_t cmd, uint8_t flags);
static void swim_dispatch_slot2(iop_t *iop);
static void swim_drive_poll_scan(iop_t *iop);
static bool swim_post_rcv2_event(iop_t *iop, uint8_t event, uint8_t drive);

// ============================================================================
//  Endian helpers — IOP RAM and host RAM are both big-endian on Mac.
// ============================================================================

static uint32_t swim_ram_read_be32(const iop_t *iop, uint32_t off) {
    return ((uint32_t)iop->ram[off] << 24) | ((uint32_t)iop->ram[off + 1] << 16) | ((uint32_t)iop->ram[off + 2] << 8) |
           (uint32_t)iop->ram[off + 3];
}

static void swim_ram_write_be16(iop_t *iop, uint32_t off, uint16_t v) {
    iop->ram[off] = (uint8_t)(v >> 8);
    iop->ram[off + 1] = (uint8_t)v;
}

static void swim_ram_write_be32(iop_t *iop, uint32_t off, uint32_t v) {
    iop->ram[off] = (uint8_t)(v >> 24);
    iop->ram[off + 1] = (uint8_t)(v >> 16);
    iop->ram[off + 2] = (uint8_t)(v >> 8);
    iop->ram[off + 3] = (uint8_t)v;
}

// ============================================================================
//  Free-list initialisation (mirrors $50BA InitMsgChannelList +
//  $51D1 InitTimerSlotList + $54F5 InitADBDriver)
// ============================================================================

// $50BA InitMsgChannelList — build 4-deep free list at $4E00 stride $10.
static void swim_init_msg_channels(iop_t *iop) {
    for (int i = 0; i < 4; i++)
        iop->ram[0x4E00 + i * 0x10] = (uint8_t)((i + 1) * 0x10);
    iop->ram[0x4E50] = 0xFF; // sentinel
    iop->ram[0x4E60] = 0x00; // free-list head = slot 0
    iop->ram[0x4E61] = 0xFF; // in-use head = empty
    iop->ram[0x4E62] = 0xFF; // reply-pending head = empty
    iop->ram[0x4E63] = 0xFF; // tx-busy slot = none
}

// $51D1 InitTimerSlotList — build 3-deep free list at $4E64 stride $08.
static void swim_init_timer_slots(iop_t *iop) {
    for (int i = 0; i < 3; i++)
        iop->ram[0x4E64 + i * 0x08] = (uint8_t)((i + 1) * 0x08);
    iop->ram[0x4E7C] = 0xFF;
    iop->ram[0x4E84] = 0x00; // free-list head = slot 0
    iop->ram[0x4E85] = 0xFF; // in-use head = empty
}

// $54F5 InitADBDriver — alloc one timer slot, fill device priority list.
static void swim_init_adb_driver(iop_t *iop) {
    // Pop the first free timer slot (=$00) and stash as $09.
    iop->ram[0x4E84] = iop->ram[0x4E64]; // advance free head
    iop->ram[0x4E65] = 0x03; // slot.state = idle
    iop->ram[0x4E68] = 0x00; // slot.callback_active = 0
    iop->ram[0x09] = 0x00; // ADB driver timer slot index = 0
    iop->ram[0x0B] = 0x0F; // bus-walk start = device 15
    // Fill device priority table $4EA9-$4EAF with identity 1..15.
    for (int x = 1; x <= 15; x++)
        iop->ram[0x4EA8 + x] = (uint8_t)x;
}

// ============================================================================
//  Main-loop heartbeat — mirrors $5029 (sentinel writes IOPAliveAddr='X')
// ============================================================================

static void swim_main_loop_tick(void *source, uint64_t data) {
    (void)data;
    iop_t *iop = (iop_t *)source;
    if (!(iop->stat_ctl & iopRunBit))
        return; // IOP held in reset; loop stops
    iop->ram[IOPAliveAddr] = 'X';
    // Re-arm.
    if (iop->scheduler)
        scheduler_new_cpu_event(iop->scheduler, &swim_main_loop_tick, iop, 0, 0, MAIN_LOOP_TICK_NS);
}

// ============================================================================
//  Drive-poll heartbeat — autonomous emission of disk-inserted /
//  -ejected events when StartPolling is in effect.  Mirrors the
//  firmware's $589E SWIM-channel poll loop, but only tracks the
//  host-visible bit: "did the OS see a disk in drive N?"
// ============================================================================

static void swim_drive_poll_tick(void *source, uint64_t data) {
    (void)data;
    iop_t *iop = (iop_t *)source;
    if (!(iop->stat_ctl & iopRunBit))
        return;
    if (!iop->ram[SWIM_MODEL_POLL_ENABLED])
        return;

    swim_drive_poll_scan(iop);

    if (iop->scheduler)
        scheduler_new_cpu_event(iop->scheduler, &swim_drive_poll_tick, iop, 0, 0, DRIVE_POLL_TICK_NS);
}

// Mapping between physical floppy module indices (0-based: 0 = internal,
// 1 = external) and the .Sony driver's drive numbers (1-based, with
// drive 0 reserved as "no drive").  See iifx-drive-numbering memory for
// background.
static inline uint8_t swim_floppy_to_drvnum(uint8_t floppy_idx) {
    return (uint8_t)(floppy_idx + 1);
}

static inline int swim_drvnum_to_floppy(uint8_t drvnum) {
    return (drvnum < 1 || drvnum > SWIM_NUM_DRIVES) ? -1 : (int)drvnum - 1;
}

// Walks both drives, comparing the current floppy_drive_image() state
// against the previous announcement and emitting the appropriate
// rcvReqDiskInserted / rcvReqDiskEjected event.  Skips drives whose
// RcvMsg[2] slot is still in flight (state != Idle) so we don't trample
// a pending event the host hasn't consumed yet.
static void swim_drive_poll_scan(iop_t *iop) {
    floppy_t *floppy = (floppy_t *)iop->bypass_device;
    if (!floppy)
        return;
    if (iop->ram[IOPRcvMsgBase + IOPMsgState(SWIM_SLOT)] != MsgIdle)
        return; // previous event not yet drained by host

    uint8_t announced = iop->ram[SWIM_MODEL_DRIVE_ANNOUNCED];
    for (uint8_t idx = 0; idx < SWIM_NUM_DRIVES; idx++) {
        bool present = floppy_is_inserted(floppy, idx);
        bool last = (announced & (1u << idx)) != 0;
        uint8_t drvnum = swim_floppy_to_drvnum(idx);
        if (present && !last) {
            if (swim_post_rcv2_event(iop, SWIM_EVT_DISK_INSERTED, drvnum)) {
                announced |= (uint8_t)(1u << idx);
                iop->ram[SWIM_MODEL_DRIVE_ANNOUNCED] = announced;
            }
            return; // one event per tick — wait for host to drain
        }
        if (!present && last) {
            if (swim_post_rcv2_event(iop, SWIM_EVT_DISK_EJECTED, drvnum)) {
                announced &= (uint8_t) ~(1u << idx);
                iop->ram[SWIM_MODEL_DRIVE_ANNOUNCED] = announced;
            }
            return;
        }
    }
}

// Helper for posting a SWIM event into RcvMsg[2].  Builds a minimal
// SwimIopMsg payload (ReqKind + DriveNumber + zero error) and uses Int1
// (the SendMsgToHost convention from $5125, which the host's IOPMgr
// translates into a level-1 IRQ at $40809B50).
static bool swim_post_rcv2_event(iop_t *iop, uint8_t event, uint8_t drive) {
    uint8_t payload[MaxIopMsgLen];
    memset(payload, 0, sizeof payload);
    payload[SIM_REQ_KIND] = event;
    payload[SIM_DRIVE_NUMBER] = drive;
    bool ok = iop_post_reply(iop, SWIM_SLOT, payload, sizeof payload, /*use_int1=*/true);
    if (ok)
        LOG(3, "SWIM IOP: RcvMsg[2] event=$%02x drive=%d", event, drive);
    return ok;
}

// ============================================================================
//  RUN-bit 0→1 — mirrors $5000 reset entry
// ============================================================================

static void iop_swim_on_run_start(iop_t *iop) {
    // $5000-$5028: stack init, $F020/$F028/$F032/$F035/$F010/$F011 = 0,
    // $F033/$F034 = $FF.  We model these as scheduler-event resets and
    // clearing our bookkeeping bytes.
    iop->ram[SWIM_MODEL_ADB_BUSY] = 0;
    iop->ram[SWIM_MODEL_ADB_CMD] = 0;
    iop->ram[SWIM_MODEL_POLL_ENABLED] = 0;
    iop->ram[SWIM_MODEL_DRIVE_PRESENT] = 0;
    iop->ram[SWIM_MODEL_DRIVE_ANNOUNCED] = 0;
    swim_ram_write_be32(iop, SWIM_MODEL_HFS_TAG_ADDR, 0);

    // $50BA InitMsgChannelList
    swim_init_msg_channels(iop);
    // $51D1 InitTimerSlotList
    swim_init_timer_slots(iop);
    // $54F5 InitADBDriver (via $53D1 InitSwimDriver → $54F5)
    swim_init_adb_driver(iop);

    // CRITICAL — do NOT overwrite IOPAliveAddr here.
    //
    // iop.c's iop_init_mailbox already wrote $FF to $031F (IOPAliveAddr).
    // The host's InitIOPMgr install path at $40804D8E-$40804D9A runs a
    // DBEQ loop (256 iterations) polling iopRamData at $031F for $FF
    // immediately after writing setIopRun.  If $031F ≠ $FF, the install
    // bails out via $40804D9C → $40804CEE without writing IOPInfoPtrs[1].
    // That would leave SWIM IOP uninstalled in IOPMgrGlobals — and every
    // subsequent _IOPMsgRequest for SWIM fails with paramErr, breaking
    // the entire ADB-via-IOP path (= the boot's $4080A8E6 fDBInit stall).
    //
    // On real hardware the firmware's main-loop heartbeat ($5029) doesn't
    // overwrite $031F with 'X' until after several hundred cycles of
    // post-reset init ($5000-$5028).  By that time the host has long
    // since finished its DBEQ-loop check.  We match that timing by
    // scheduling the main-loop heartbeat far enough out that the host's
    // install completes first.

    LOG(2, "SWIM IOP: firmware-equivalent state machine started "
           "(leaving IOPAliveAddr=$FF for host install path)");

    // Schedule main-loop heartbeat.  First tick fires after MAIN_LOOP_TICK_NS
    // (10 ms) — well past the host's ~256-iteration alive-poll window.
    if (iop->scheduler) {
        remove_event(iop->scheduler, &swim_main_loop_tick, iop);
        remove_event(iop->scheduler, &swim_adb_response, iop);
        remove_event(iop->scheduler, &swim_drive_poll_tick, iop);
        scheduler_new_cpu_event(iop->scheduler, &swim_main_loop_tick, iop, 0, 0, MAIN_LOOP_TICK_NS);
    }
}

// ============================================================================
//  Host-kick handler — mirrors $5070
//
// Walk RcvMsg[1..7] for MsgCompleted (host drained our reply) and
// XmtMsg[1..7] for NewMsgSent (host posted a request).  Dispatch each
// via the per-slot handler.
// ============================================================================

static void iop_swim_on_host_kick(iop_t *iop) {
    // $5080 loop: scan RcvMsg for MsgCompleted, dispatch "drained" handler
    // (per-channel "$7D3E,x" table).  We model: clear RcvMsg state to Idle,
    // and on SWIM_SLOT (2) re-check the drive-poll state so any pending
    // disk-inserted event can be re-armed once the previous one drains.
    for (int slot = 1; slot <= MaxIopMsgNum; slot++) {
        if (iop->ram[IOPRcvMsgBase + IOPMsgState(slot)] == MsgCompleted)
            swim_handle_rcv_drain(iop, slot);
    }
    // $5099 loop: scan XmtMsg for NewMsgSent, dispatch "new msg" handler
    // (per-channel "$7D4C,x" table).  For slot 2 (SWIM/floppy) this enters
    // the swim_dispatch_slot2 router; for slot 3 (ADB) we start a packet
    // send and reply path; other slots are stub-acked.
    for (int slot = 1; slot <= MaxIopMsgNum; slot++) {
        if (iop->ram[IOPXmtMsgBase + IOPMsgState(slot)] == NewMsgSent)
            swim_handle_xmt_slot(iop, slot);
    }
}

// ============================================================================
//  Slot dispatcher — chooses per-channel handler
// ============================================================================

static void swim_handle_xmt_slot(iop_t *iop, int slot) {
    if (slot == SWIM_SLOT) {
        swim_dispatch_slot2(iop);
        return;
    }
    if (slot == ADB_SLOT) {
        // Slot 3 (ADB): read the ADBMsg payload, ack and bit-bang.
        uint32_t pl = IOPMsgPayload(IOPXmtMsgBase, slot);
        uint8_t flags = iop->ram[pl + ADBMSG_FLAGS];
        uint8_t count = iop->ram[pl + ADBMSG_DATACOUNT];
        uint8_t cmd = iop->ram[pl + ADBMSG_ADBCMD];
        LOG(3, "SWIM IOP: XmtMsg[3] ADB req: flags=$%02x count=%d cmd=$%02x", flags, count, cmd);

        // $550D ack: XmtMsg[3].state := MsgCompleted, raise Int0.
        iop->ram[IOPXmtMsgBase + IOPMsgState(slot)] = MsgCompleted;
        iop_raise_int0(iop);

        // $5560 → $5610: start ADB packet send.
        swim_start_adb_send(iop, cmd, flags);
        return;
    }
    // Slots 1, 4..7 are reserved.  Ack so the host's level-1 IRQ
    // unblocks; if a host driver ever posts here it'll see MsgCompleted
    // with whatever payload it sent and (we hope) treat the buffer's
    // ReqKind=0 echo as a noop.
    iop->ram[IOPXmtMsgBase + IOPMsgState(slot)] = MsgCompleted;
    iop_raise_int0(iop);
    LOG(3, "SWIM IOP: XmtMsg[%d] (unhandled channel) — acking with no work", slot);
}

// ============================================================================
//  Slot 2 (SWIM / floppy) dispatcher
//
// All commands take their parameters from the XmtMsg[2] payload starting
// at IOPMsgPayload(IOPXmtMsgBase, 2) = $0240.  Replies are written back
// into the same buffer (the .Sony driver's _IOPMsgRequest uses the same
// pointer for irMessagePtr and irReplyPtr — see SonyIOP.a's
// initialization at $40824AE0 in the host ROM).  When the IOP marks the
// slot MsgCompleted + raises Int0, the host's @CallIOPsync waiter wakes
// up and the driver reads the reply from the same XmtMsg[2] buffer.
// ============================================================================

static void swim_slot2_complete(iop_t *iop, int16_t error) {
    uint32_t pl = IOPMsgPayload(IOPXmtMsgBase, SWIM_SLOT);
    swim_ram_write_be16(iop, pl + SIM_ERROR_CODE, (uint16_t)error);
    iop->ram[IOPXmtMsgBase + IOPMsgState(SWIM_SLOT)] = MsgCompleted;
    iop_raise_int0(iop);
}

// Fills SwimIopMsg.DriveKinds[0..3] (offset 4..7) with the kinds of the
// drives currently wired through bypass_device.  We model two physical
// drives (internal HD floppy, external HD floppy); slots beyond that
// report DRIVE_KIND_NONE.
static void swim_handle_initialize(iop_t *iop) {
    uint32_t pl = IOPMsgPayload(IOPXmtMsgBase, SWIM_SLOT);
    floppy_t *floppy = (floppy_t *)iop->bypass_device;
    (void)floppy; // present-or-not at this stage doesn't change the kind

    // Zero the AdditionalParam union area first so stale bytes from
    // previous calls don't leak into the reply.
    for (int i = 0; i < 8; i++)
        iop->ram[pl + SIM_DRIVE_KINDS + i] = 0;

    // The IIfx ships two SuperDrives (1.44 MB MFM-capable GCR).  Both
    // slots advertise DRIVE_KIND_DSMFM_HD; slot 0 is left as
    // DRIVE_KIND_NONE so the .Sony driver's @AddDriveLoop skips d1=0
    // (the "logical drive 0" position) when calling _AddDrive.  This
    // shift is required because the .Sony driver passes d1 directly as
    // the drive number argument to _AddDrive — and Mac OS treats drive
    // number 0 as "no drive" / invalid, so the first physical floppy
    // must be registered at d1=1 to receive qDrive=1 in the drive
    // queue.  Without this shift, _MountVol(BootDrive=0) returns
    // paramErr (-50) and the boot block code at $00400FA2 aborts
    // (see local/gs-docs/asm/IIfx-ROM.asm §23 for the analysis).
    iop->ram[pl + SIM_DRIVE_KINDS + 0] = DRIVE_KIND_NONE;
    iop->ram[pl + SIM_DRIVE_KINDS + 1] = DRIVE_KIND_DSMFM_HD;
    iop->ram[pl + SIM_DRIVE_KINDS + 2] = DRIVE_KIND_DSMFM_HD;
    iop->ram[pl + SIM_DRIVE_KINDS + 3] = DRIVE_KIND_NONE;

    LOG(3, "SWIM IOP: Initialize — DriveKinds = {%02x %02x %02x %02x}", iop->ram[pl + SIM_DRIVE_KINDS + 0],
        iop->ram[pl + SIM_DRIVE_KINDS + 1], iop->ram[pl + SIM_DRIVE_KINDS + 2], iop->ram[pl + SIM_DRIVE_KINDS + 3]);

    swim_slot2_complete(iop, MAC_ERR_NO_ERR);
}

// Resets the announce bitmap so the next poll-tick re-emits inserted
// events for whatever's currently in the drives, and arms the autonomous
// drive-poll timer.
static void swim_handle_start_polling(iop_t *iop) {
    iop->ram[SWIM_MODEL_POLL_ENABLED] = 1;
    iop->ram[SWIM_MODEL_DRIVE_ANNOUNCED] = 0;
    LOG(3, "SWIM IOP: StartPolling — autonomous drive scan enabled");

    if (iop->scheduler) {
        remove_event(iop->scheduler, &swim_drive_poll_tick, iop);
        scheduler_new_cpu_event(iop->scheduler, &swim_drive_poll_tick, iop, 0, 0, DRIVE_POLL_TICK_NS);
    }
    swim_slot2_complete(iop, MAC_ERR_NO_ERR);
}

static void swim_handle_stop_polling(iop_t *iop) {
    iop->ram[SWIM_MODEL_POLL_ENABLED] = 0;
    LOG(3, "SWIM IOP: StopPolling — autonomous drive scan disabled");
    if (iop->scheduler)
        remove_event(iop->scheduler, &swim_drive_poll_tick, iop);
    swim_slot2_complete(iop, MAC_ERR_NO_ERR);
}

static void swim_handle_set_hfs_tag(iop_t *iop) {
    uint32_t pl = IOPMsgPayload(IOPXmtMsgBase, SWIM_SLOT);
    uint32_t addr = swim_ram_read_be32(iop, pl + SIM_BUFFER_ADDR);
    swim_ram_write_be32(iop, SWIM_MODEL_HFS_TAG_ADDR, addr);
    LOG(3, "SWIM IOP: SetHFSTagAddr — host MFS-tag buffer at $%08x", addr);
    swim_slot2_complete(iop, MAC_ERR_NO_ERR);
}

// Reads the .Sony driver's 1-based drive number from XmtMsg[2], maps it
// to a 0-based floppy module index, and validates the range.  Returns
// false (with *out_err set to NSDrvErr) if the drive number is 0 or
// past the number of physical floppy bays we model; caller should mark
// the slot completed with that error.
static bool swim_validate_drive(iop_t *iop, int *out_floppy_idx, uint8_t *out_drvnum, int16_t *out_err) {
    uint32_t pl = IOPMsgPayload(IOPXmtMsgBase, SWIM_SLOT);
    uint8_t drvnum = iop->ram[pl + SIM_DRIVE_NUMBER];
    *out_drvnum = drvnum;
    int idx = swim_drvnum_to_floppy(drvnum);
    if (idx < 0) {
        *out_floppy_idx = -1;
        *out_err = MAC_ERR_NS_DRV;
        return false;
    }
    *out_floppy_idx = idx;
    *out_err = MAC_ERR_NO_ERR;
    return true;
}

// Fills the DriveStatus and ExtDriveStatus fields of XmtMsg[2] from the
// floppy module's live state.  Mirrors the firmware's $58xx status-build
// path: track, write-protect, in-place, installed, sides, format.
// `floppy_idx` is 0-based (already translated from the .Sony driver's
// 1-based drive number by swim_validate_drive).
static void swim_fill_drive_status(iop_t *iop, int floppy_idx) {
    uint32_t pl = IOPMsgPayload(IOPXmtMsgBase, SWIM_SLOT);
    floppy_t *floppy = (floppy_t *)iop->bypass_device;
    image_t *img = (floppy && floppy_idx >= 0) ? floppy_drive_image(floppy, (unsigned)floppy_idx) : NULL;
    bool present = img != NULL;
    bool writable = present && img->writable;
    bool is_hd = present && img->type == image_fd_hd;
    bool is_ds = present && (img->type == image_fd_ds || img->type == image_fd_hd);

    int track = (floppy && floppy_idx >= 0) ? floppy_drive_track(floppy, (unsigned)floppy_idx) : 0;
    if (track < 0)
        track = 0;
    swim_ram_write_be16(iop, pl + SIM_TRACK, (uint16_t)track);
    iop->ram[pl + SIM_WRITE_PROTECTED] = (uint8_t)(writable ? 0x00 : 0x80);
    iop->ram[pl + SIM_DISK_IN_PLACE] = (uint8_t)(present ? 0x02 : 0x00);
    iop->ram[pl + SIM_DRIVE_INSTALLED] = 0x01; // we always have the drive bay
    iop->ram[pl + SIM_SIDES] = (uint8_t)(is_ds ? 0x80 : 0x00);
    iop->ram[pl + SIM_TWO_SIDED_FORMAT] = (uint8_t)(is_ds ? 0xFF : 0x00);
    iop->ram[pl + SIM_NEW_INTERFACE] = 0xFF; // SuperDrive-class interface
    swim_ram_write_be16(iop, pl + SIM_DISK_ERRORS, 0);
    iop->ram[pl + SIM_MFM_DRIVE] = 0xFF; // is a SuperDrive
    iop->ram[pl + SIM_MFM_DISK] = (uint8_t)(is_hd ? 0xFF : 0x00);
    iop->ram[pl + SIM_MFM_FORMAT] = (uint8_t)(is_hd ? 0xFF : 0x00);
    iop->ram[pl + SIM_DISK_CONTROLLER] = 0xFF; // is a SWIM, not IWM
    // Per IM:VI §3 "GetFormatList" — bit masks of supported GCR/MFM
    // capacities.  Bit 0 = 400K, bit 1 = 800K, bit 2 = 720K, bit 3 = 1440K.
    uint16_t allowed = 0;
    uint16_t current = 0;
    if (present) {
        if (img->type == image_fd_ss) {
            allowed = 0x0001;
            current = 0x0001;
        } else if (img->type == image_fd_ds) {
            allowed = 0x0003;
            current = 0x0002;
        } else if (img->type == image_fd_hd) {
            allowed = 0x000F;
            current = 0x0008;
        }
    } else {
        // Empty SuperDrive can accept anything in the SuperDrive family.
        allowed = 0x000F;
    }
    swim_ram_write_be16(iop, pl + SIM_CURRENT_FORMAT, current);
    swim_ram_write_be16(iop, pl + SIM_FORMATS_ALLOWED, allowed);
}

static void swim_handle_drive_status(iop_t *iop) {
    int idx;
    uint8_t drvnum;
    int16_t err;
    if (!swim_validate_drive(iop, &idx, &drvnum, &err)) {
        swim_slot2_complete(iop, err);
        return;
    }
    swim_fill_drive_status(iop, idx);
    LOG(3, "SWIM IOP: DriveStatus drive=%d present=%d", drvnum,
        iop->ram[IOPMsgPayload(IOPXmtMsgBase, SWIM_SLOT) + SIM_DISK_IN_PLACE] != 0);
    swim_slot2_complete(iop, MAC_ERR_NO_ERR);
}

static void swim_handle_eject(iop_t *iop) {
    int idx;
    uint8_t drvnum;
    int16_t err;
    if (!swim_validate_drive(iop, &idx, &drvnum, &err)) {
        swim_slot2_complete(iop, err);
        return;
    }
    floppy_t *floppy = (floppy_t *)iop->bypass_device;
    if (floppy && idx >= 0)
        (void)floppy_drive_eject(floppy, (unsigned)idx);
    if (idx >= 0)
        iop->ram[SWIM_MODEL_DRIVE_ANNOUNCED] &= (uint8_t) ~(1u << idx);
    LOG(3, "SWIM IOP: Eject drive=%d", drvnum);
    swim_slot2_complete(iop, MAC_ERR_NO_ERR);
}

// Resolves host_addr to a direct RAM pointer using ram_native_pointer.
// The IIfx PIC's DMA controller writes to *physical* RAM addresses,
// bypassing the host MMU — modelled here by writing directly into the
// memory_map_t's flat RAM image.  Returns NULL if host_addr + byte_count
// would extend past the configured RAM size.
static uint8_t *swim_host_dma_ptr(uint32_t host_addr, size_t byte_count) {
    config_t *cfg = global_emulator;
    if (!cfg || !cfg->mem_map)
        return NULL;
    if ((uint64_t)host_addr + byte_count > (uint64_t)cfg->ram_size)
        return NULL;
    return ram_native_pointer(cfg->mem_map, host_addr);
}

// Copies `count` blocks (512 bytes each) starting at `block_number` from
// the floppy image at `floppy_idx` (0-based) into host RAM at `host_addr`.
// Returns a MacOS-level error code (0 on success, offLinErr/paramErr on
// failure).
static int16_t swim_read_blocks(iop_t *iop, int floppy_idx, uint32_t block_number, uint32_t count, uint32_t host_addr) {
    floppy_t *floppy = (floppy_t *)iop->bypass_device;
    image_t *img = (floppy && floppy_idx >= 0) ? floppy_drive_image(floppy, (unsigned)floppy_idx) : NULL;
    if (!img)
        return MAC_ERR_OFFLINE;

    size_t total = disk_size(img);
    size_t byte_offset = (size_t)block_number * 512u;
    size_t byte_count = (size_t)count * 512u;
    if (byte_offset + byte_count > total)
        return MAC_ERR_PARAM;

    uint8_t *dst = swim_host_dma_ptr(host_addr, byte_count);
    if (!dst)
        return MAC_ERR_PARAM;
    size_t got = disk_read_data(img, byte_offset, dst, byte_count);
    if (got != byte_count)
        return MAC_ERR_IO;
    return MAC_ERR_NO_ERR;
}

static int16_t swim_write_blocks(iop_t *iop, int floppy_idx, uint32_t block_number, uint32_t count,
                                 uint32_t host_addr) {
    floppy_t *floppy = (floppy_t *)iop->bypass_device;
    image_t *img = (floppy && floppy_idx >= 0) ? floppy_drive_image(floppy, (unsigned)floppy_idx) : NULL;
    if (!img)
        return MAC_ERR_OFFLINE;
    if (!img->writable)
        return MAC_ERR_W_PR;

    size_t total = disk_size(img);
    size_t byte_offset = (size_t)block_number * 512u;
    size_t byte_count = (size_t)count * 512u;
    if (byte_offset + byte_count > total)
        return MAC_ERR_PARAM;

    uint8_t *src = swim_host_dma_ptr(host_addr, byte_count);
    if (!src)
        return MAC_ERR_PARAM;
    size_t wrote = disk_write_data(img, byte_offset, src, byte_count);
    if (wrote != byte_count)
        return MAC_ERR_IO;
    return MAC_ERR_NO_ERR;
}

static void swim_handle_read(iop_t *iop, bool verify_only) {
    int idx;
    uint8_t drvnum;
    int16_t err;
    if (!swim_validate_drive(iop, &idx, &drvnum, &err)) {
        swim_slot2_complete(iop, err);
        return;
    }
    uint32_t pl = IOPMsgPayload(IOPXmtMsgBase, SWIM_SLOT);
    uint32_t buf = swim_ram_read_be32(iop, pl + SIM_BUFFER_ADDR);
    uint32_t blk = swim_ram_read_be32(iop, pl + SIM_BLOCK_NUMBER);
    uint32_t cnt = swim_ram_read_be32(iop, pl + SIM_BLOCK_COUNT);
    int16_t rc;
    if (verify_only) {
        // Verify mode: read but don't store.  We have no on-disk error
        // model, so just confirm the range is valid.
        floppy_t *floppy = (floppy_t *)iop->bypass_device;
        image_t *img = (floppy && idx >= 0) ? floppy_drive_image(floppy, (unsigned)idx) : NULL;
        if (!img)
            rc = MAC_ERR_OFFLINE;
        else if ((size_t)(blk + cnt) * 512u > disk_size(img))
            rc = MAC_ERR_PARAM;
        else
            rc = MAC_ERR_NO_ERR;
    } else {
        rc = swim_read_blocks(iop, idx, blk, cnt, buf);
    }
    LOG(3, "SWIM IOP: %s drive=%d blk=%u cnt=%u buf=$%08x → %d", verify_only ? "ReadVerify" : "Read", drvnum, blk, cnt,
        buf, rc);
    swim_slot2_complete(iop, rc);
}

static void swim_handle_write(iop_t *iop) {
    int idx;
    uint8_t drvnum;
    int16_t err;
    if (!swim_validate_drive(iop, &idx, &drvnum, &err)) {
        swim_slot2_complete(iop, err);
        return;
    }
    uint32_t pl = IOPMsgPayload(IOPXmtMsgBase, SWIM_SLOT);
    uint32_t buf = swim_ram_read_be32(iop, pl + SIM_BUFFER_ADDR);
    uint32_t blk = swim_ram_read_be32(iop, pl + SIM_BLOCK_NUMBER);
    uint32_t cnt = swim_ram_read_be32(iop, pl + SIM_BLOCK_COUNT);
    int16_t rc = swim_write_blocks(iop, idx, blk, cnt, buf);
    LOG(3, "SWIM IOP: Write drive=%d blk=%u cnt=%u buf=$%08x → %d", drvnum, blk, cnt, buf, rc);
    swim_slot2_complete(iop, rc);
}

// Format / FormatVerify: we don't simulate sector-by-sector formatting;
// just clear the image (Format) or treat as a no-op (FormatVerify).
static void swim_handle_format(iop_t *iop, bool verify_only) {
    int idx;
    uint8_t drvnum;
    int16_t err;
    if (!swim_validate_drive(iop, &idx, &drvnum, &err)) {
        swim_slot2_complete(iop, err);
        return;
    }
    floppy_t *floppy = (floppy_t *)iop->bypass_device;
    image_t *img = (floppy && idx >= 0) ? floppy_drive_image(floppy, (unsigned)idx) : NULL;
    if (!img) {
        swim_slot2_complete(iop, MAC_ERR_OFFLINE);
        return;
    }
    int16_t rc = MAC_ERR_NO_ERR;
    if (!verify_only) {
        if (!img->writable) {
            rc = MAC_ERR_W_PR;
        } else {
            uint8_t zeros[512];
            memset(zeros, 0, sizeof zeros);
            size_t total = disk_size(img);
            for (size_t off = 0; off < total; off += 512) {
                if (disk_write_data(img, off, zeros, sizeof zeros) != sizeof zeros) {
                    rc = MAC_ERR_IO;
                    break;
                }
            }
        }
    }
    LOG(3, "SWIM IOP: %s drive=%d → %d", verify_only ? "FormatVerify" : "Format", drvnum, rc);
    swim_slot2_complete(iop, rc);
}

static void swim_dispatch_slot2(iop_t *iop) {
    uint32_t pl = IOPMsgPayload(IOPXmtMsgBase, SWIM_SLOT);
    uint8_t req = iop->ram[pl + SIM_REQ_KIND];

    switch (req) {
    case SWIM_REQ_INITIALIZE:
        swim_handle_initialize(iop);
        break;
    case SWIM_REQ_SHUTDOWN:
        swim_slot2_complete(iop, MAC_ERR_NO_ERR);
        break;
    case SWIM_REQ_START_POLLING:
        swim_handle_start_polling(iop);
        break;
    case SWIM_REQ_STOP_POLLING:
        swim_handle_stop_polling(iop);
        break;
    case SWIM_REQ_SET_HFS_TAG:
        swim_handle_set_hfs_tag(iop);
        break;
    case SWIM_REQ_DRIVE_STATUS:
        swim_handle_drive_status(iop);
        break;
    case SWIM_REQ_EJECT:
        swim_handle_eject(iop);
        break;
    case SWIM_REQ_FORMAT:
        swim_handle_format(iop, false);
        break;
    case SWIM_REQ_FORMAT_VERIFY:
        swim_handle_format(iop, true);
        break;
    case SWIM_REQ_WRITE:
        swim_handle_write(iop);
        break;
    case SWIM_REQ_READ:
        swim_handle_read(iop, false);
        break;
    case SWIM_REQ_READ_VERIFY:
        swim_handle_read(iop, true);
        break;
    case SWIM_REQ_CACHE_CONTROL:
    case SWIM_REQ_TAG_BUF_CONTROL:
        // No internal cache or tag-buffer state to manage; accept silently.
        swim_slot2_complete(iop, MAC_ERR_NO_ERR);
        break;
    case SWIM_REQ_GET_ICON:
        // No driver-resident icon resource; .Sony falls back to a default
        // in the host driver when the IOP returns ParamErr here.
        swim_slot2_complete(iop, MAC_ERR_PARAM);
        break;
    case SWIM_REQ_DUP_INFO:
    case SWIM_REQ_GET_RAW_DATA:
        // Disk-Duplicator extensions; report "not supported" via paramErr.
        swim_slot2_complete(iop, MAC_ERR_PARAM);
        break;
    default:
        LOG(2, "SWIM IOP: XmtMsg[2] unknown req=$%02x — returning paramErr", req);
        swim_slot2_complete(iop, MAC_ERR_PARAM);
        break;
    }
}

// ============================================================================
//  ADB packet send — mirrors $5610 + $5701
//
// In real firmware this bit-bangs the cmd byte onto the ADB single-wire
// line via $F032 and then samples the line for a response.  We abstract
// this as "schedule a response event in ADB_TIMEOUT_NS".  When the event
// fires, swim_adb_response builds the reply based on whatever the ADB
// bus model returned (currently always NoReply since no devices are
// wired through this transport).
// ============================================================================

static void swim_start_adb_send(iop_t *iop, uint8_t cmd, uint8_t flags) {
    iop->ram[SWIM_MODEL_ADB_BUSY] = 1;
    iop->ram[SWIM_MODEL_ADB_CMD] = cmd;
    iop->ram[SWIM_MODEL_ADB_FLAGS] = flags; // save for reply
    // Echo the cmd into $4EA0 like $5610 does (used by the reply builder).
    iop->ram[0x4EA0] = cmd;

    if (iop->scheduler) {
        remove_event(iop->scheduler, &swim_adb_response, iop);
        scheduler_new_cpu_event(iop->scheduler, &swim_adb_response, iop, 0, 0, ADB_TIMEOUT_NS);
    }
}

// ============================================================================
//  ADB response handler — mirrors the tail of $5560 (after $5610 returns)
//
// At this point the firmware has finished its bit-bang attempt.  For a
// "no device" outcome, $07 = NoReply ($02) and $08 = 0 (DataCount).
// Build the reply in RcvMsg[3] payload and raise Int1.
// ============================================================================

static void swim_adb_response(void *source, uint64_t data) {
    (void)data;
    iop_t *iop = (iop_t *)source;
    if (!(iop->stat_ctl & iopRunBit))
        return;
    if (!iop->ram[SWIM_MODEL_ADB_BUSY])
        return;

    // $5560: if RcvMsg[3].state != Idle (host hasn't drained previous
    // reply), defer and retry after a short delay.
    if (iop->ram[IOPRcvMsgBase + IOPMsgState(ADB_SLOT)] != MsgIdle) {
        if (iop->scheduler)
            scheduler_new_cpu_event(iop->scheduler, &swim_adb_response, iop, 0, 0, 100000ULL);
        return;
    }

    // Build the reply.  The firmware's $5560 channel engine writes its
    // internal $07 state directly to RcvMsg[N].Flags — that state was
    // initialised from the request's Flags byte in $55B4 (`sta $07`)
    // and then ORed with NoReply ($02) in $5610.  So the reply's Flags
    // = request_Flags | NoReply.  Critically, the ExplicitCmd bit
    // (bit 7) from the request must be preserved so the host routes
    // via ExplicitRequestDone (which advances the cmd queue head)
    // rather than ImplicitRequestDone (which leaves the queue stuck).
    uint8_t reply_flags = iop->ram[SWIM_MODEL_ADB_FLAGS] | ADBMSG_FLAG_NOREPLY;
    uint32_t pl = IOPMsgPayload(IOPRcvMsgBase, ADB_SLOT);
    iop->ram[pl + ADBMSG_FLAGS] = reply_flags;
    iop->ram[pl + ADBMSG_DATACOUNT] = 0;
    iop->ram[pl + ADBMSG_ADBCMD] = iop->ram[SWIM_MODEL_ADB_CMD];
    // ADBData bytes left as-is — host won't read them when DataCount==0.

    // $5560: increment RcvMsg[3].state to NewMsgSent and raise Int1.
    iop->ram[IOPRcvMsgBase + IOPMsgState(ADB_SLOT)] = NewMsgSent;
    iop_raise_int1(iop);

    iop->ram[SWIM_MODEL_ADB_BUSY] = 0;
    LOG(3, "SWIM IOP: RcvMsg[3] ADB reply: NoReply, cmd echo=$%02x", iop->ram[SWIM_MODEL_ADB_CMD]);
}

// ============================================================================
//  RcvMsg drain handler — mirrors per-channel "$7D3E,x" routines
//
// For slot 2 (SWIM): the host's IOPMgr writes RcvMsg[2].state =
// MsgCompleted after its ReceivedCallFromIop handler in SonyIOP.a has
// posted DiskInsertEvt / DiskEjectEvt and re-issued irSendRcvReply.
// We clear the state to Idle and re-run the drive-poll scan in case
// another event is queued.
//
// For slot 3 (ADB): RcvMsg[3] has a dual role:
//   - When the host first posts a request (BusReset), it uses XmtMsg[3]
//     because fDBUseRcvMsg is initially 0.
//   - After the first reply, the ADB Manager's IOP completion handler
//     sets fDBUseRcvMsg=1.  ALL subsequent requests then go through
//     RcvMsg[3] via _IOPMsgRequest's @SendRcvReply path, which writes
//     RcvMsg[3].state = MsgCompleted along with new payload data.
//
// The firmware's $5524 distinguishes "new request" from "drain ack" by
// inspecting the payload's Flags byte ($55B4 logic): if Flags has bit 7
// (ExplicitCmd) or bit 5 (SetPollEnables), it's a real request; if all
// these bits are 0, it's a pure drain ack and the firmware just clears
// the state.
// ============================================================================

static void swim_handle_rcv_slot(iop_t *iop, int slot);

static void swim_handle_rcv_drain(iop_t *iop, int slot) {
    if (slot == ADB_SLOT) {
        // For ADB slot, route through the per-slot handler which
        // distinguishes new request vs drain ack by Flags inspection.
        swim_handle_rcv_slot(iop, slot);
        return;
    }
    if (slot == SWIM_SLOT) {
        // Host drained our previous DiskInserted/Ejected event.
        iop->ram[IOPRcvMsgBase + IOPMsgState(slot)] = MsgIdle;
        LOG(3, "SWIM IOP: RcvMsg[2] drained — re-running drive-poll scan");
        if (iop->ram[SWIM_MODEL_POLL_ENABLED])
            swim_drive_poll_scan(iop);
        return;
    }
    iop->ram[IOPRcvMsgBase + IOPMsgState(slot)] = MsgIdle;
    LOG(3, "SWIM IOP: RcvMsg[%d] drained by host (state := Idle)", slot);
}

// $5524 mirror — slot-3 RcvMsg MsgCompleted handler.  Reads payload,
// dispatches to the same code as XmtMsg new-msg.  Inspects Flags to
// distinguish a real request (ExplicitCmd or SetPollEnables bit) from
// a pure drain ack (no flag bits).
static void swim_handle_rcv_slot(iop_t *iop, int slot) {
    uint32_t pl = IOPMsgPayload(IOPRcvMsgBase, slot);
    uint8_t flags = iop->ram[pl + ADBMSG_FLAGS];
    uint8_t count = iop->ram[pl + ADBMSG_DATACOUNT];
    uint8_t cmd = iop->ram[pl + ADBMSG_ADBCMD];

    // Clear state to Idle (per firmware $552F: STZ $0303).
    iop->ram[IOPRcvMsgBase + IOPMsgState(slot)] = MsgIdle;

    // Inspect Flags: if neither ExplicitCmd nor SetPollEnables, this is
    // a pure drain ack (no actual request data).
    if ((flags & (ADBMSG_FLAG_EXPLICIT | ADBMSG_FLAG_SETPOLL_EN)) == 0) {
        LOG(3, "SWIM IOP: RcvMsg[%d] drain ack (Flags=$%02x — no request data)", slot, flags);
        return;
    }

    LOG(3, "SWIM IOP: RcvMsg[%d] ADB req (irSendRcvReply path): flags=$%02x count=%d cmd=$%02x", slot, flags, count,
        cmd);

    // Process the request the same way as an XmtMsg-side request.
    swim_start_adb_send(iop, cmd, flags);
}

// ============================================================================
//  Behaviour table
// ============================================================================

const iop_behavior_t iop_swim_behavior = {
    .name = "SWIM IOP",
    .kind = SwimIopNum,
    // FNV-1a32 of iop-swim.bin captured from a IIfx ROM boot.
    .expected_fnv1a = 0x10fd18fdu,
    .on_run_start = iop_swim_on_run_start,
    .on_host_kick = iop_swim_on_host_kick,
};
