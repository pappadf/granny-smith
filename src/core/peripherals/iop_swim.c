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
// Out-of-band state (scheduler-event tracking) is implicit in whether
// scheduler events are armed; we use remove_event before each arm to
// avoid double-scheduling.
//
// Translation strategy
// --------------------
// The firmware is interrupt-driven with these external trigger points:
//
//   1. RUN-bit 0→1     →  iop_swim_on_run_start  (mirrors $5000 entry)
//   2. Host-kick IRQ   →  iop_swim_on_host_kick  (mirrors $5070)
//   3. Timer expiry    →  swim_timer_expire      (mirrors $5331)
//   4. Main-loop tick  →  swim_main_loop_tick    (mirrors $5029 body)
//   5. ADB cmd complete → swim_adb_response     (the host-visible result
//                          of running the firmware's $5610 bit-bang loop)
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

#include "iop_internal.h"

#include "log.h"
#include "scheduler.h"

#include <string.h>

LOG_USE_CATEGORY_NAME("iop_swim");

// ============================================================================
//  Constants matching the firmware's protocol
// ============================================================================

// Channel assignments (IOP Manager + ADB Manager conventions)
#define ADB_SLOT 3 // ADBMsgNum = 3 — slot 3 carries ADB packets

// ADBMsg structure offsets within an XmtMsg / RcvMsg payload
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
#define SWIM_MODEL_BASE      0x4FE0
#define SWIM_MODEL_ADB_BUSY  (SWIM_MODEL_BASE + 0) // 1 if ADB cmd in flight
#define SWIM_MODEL_ADB_CMD   (SWIM_MODEL_BASE + 1) // echoed back in reply
#define SWIM_MODEL_ADB_FLAGS (SWIM_MODEL_BASE + 2) // request Flags saved for reply

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
#define ADB_TIMEOUT_NS    1000000ULL //  1 ms
#define MAIN_LOOP_TICK_NS 10000000ULL // 10 ms

// ============================================================================
//  Forward declarations
// ============================================================================

static void swim_main_loop_tick(void *source, uint64_t data);
static void swim_adb_response(void *source, uint64_t data);
static void swim_handle_xmt_slot(iop_t *iop, int slot);
static void swim_handle_rcv_drain(iop_t *iop, int slot);
static void swim_start_adb_send(iop_t *iop, uint8_t cmd, uint8_t flags);

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
//  RUN-bit 0→1 — mirrors $5000 reset entry
// ============================================================================

static void iop_swim_on_run_start(iop_t *iop) {
    // $5000-$5028: stack init, $F020/$F028/$F032/$F035/$F010/$F011 = 0,
    // $F033/$F034 = $FF.  We model these as scheduler-event resets and
    // clearing our bookkeeping bytes.
    iop->ram[SWIM_MODEL_ADB_BUSY] = 0;
    iop->ram[SWIM_MODEL_ADB_CMD] = 0;

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
    // (per-channel "$7D3E,x" table).  We model: clear RcvMsg state to Idle.
    for (int slot = 1; slot <= MaxIopMsgNum; slot++) {
        if (iop->ram[IOPRcvMsgBase + IOPMsgState(slot)] == MsgCompleted)
            swim_handle_rcv_drain(iop, slot);
    }
    // $5099 loop: scan XmtMsg for NewMsgSent, dispatch "new msg" handler
    // (per-channel "$7D4C,x" table).  For slot 3 (ADB), this triggers
    // an ADB packet send.
    for (int slot = 1; slot <= MaxIopMsgNum; slot++) {
        if (iop->ram[IOPXmtMsgBase + IOPMsgState(slot)] == NewMsgSent)
            swim_handle_xmt_slot(iop, slot);
    }
}

// ============================================================================
//  Slot 3 NewMsg handler — mirrors $550D + $55B4 + $5560
//
// Read the request from XmtMsg[3] payload (ADBMsg layout: Flags,
// DataCount, ADBCmd, ADBData[0..7]), ack the XmtMsg, and kick off the
// ADB packet send.
// ============================================================================

static void swim_handle_xmt_slot(iop_t *iop, int slot) {
    if (slot != ADB_SLOT) {
        // Slot 2 (SWIM floppy) and slots 4-7 not modelled yet — ack as
        // MsgCompleted so the host's IOPInterrupt unblocks.  This is
        // safe because the floppy paths aren't exercised during boot.
        iop->ram[IOPXmtMsgBase + IOPMsgState(slot)] = MsgCompleted;
        iop_raise_int0(iop);
        LOG(3, "SWIM IOP: XmtMsg[%d] (floppy/other) — acking with no work", slot);
        return;
    }

    // Slot 3 (ADB): read the ADBMsg payload.
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
// For slot 3 (ADB) this is $5524.  In the IopADB protocol, RcvMsg[3]
// has a dual role:
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
