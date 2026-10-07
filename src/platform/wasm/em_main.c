// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// em_main.c
// Main Emscripten platform implementation - handles main loop, input, checkpointing, and filesystem commands

// ============================================================================
// Includes
// ============================================================================

#include "em.h"
#include "io/io_worker.h"
#include "job/job.h"

#include <assert.h>
#include <ctype.h>
#include <emscripten.h>
#include <emscripten/atomic.h>
#include <emscripten/emscripten.h>
#include <emscripten/html5.h>
#include <emscripten/threading.h>
#include <emscripten/version.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

#include <emscripten/stack.h>
#include <emscripten/wasmfs.h>

#include "api.h"
#include "appletalk.h"
#include "checkpoint.h"
#include "checkpoint_machine.h"
#include "cpu.h"
#include "host_keys.h"
#include "keyboard.h"
#include "laserwriter_job.h"
#include "laserwriter_transport.h"
#include "log.h"
#include "machine.h"
#include "mouse.h"
#include "platform.h"
#include "printer_sink.h"
#include "prom.h"
#include "scheduler.h"
#include "shell.h"
#include "storage_util.h"
#include "system.h"
#include "vrom.h"

// ============================================================================
// Forward Declarations
// ============================================================================

static void em_assertion_callback(const char *kind, const char *expr, const char *file, int line, const char *func);

// The always-present AppleShare volume.  The path literal lives here, in the
// platform layer, because core never fabricates or interprets a path (PR #69);
// the AppleTalk network publishes it once, at startup
// (system_set_default_share), for every machine that plugs in.
// Under OPFS the directory — and the AppleDouble sidecars the AFP server
// writes beside each file — persist across page reloads for free.
#define GS_DEFAULT_SHARE_PATH "/opfs/shared"

// ============================================================================
// Pointer-lock and Input Handling
// ============================================================================

static volatile int pointer_locked = 0;
static bool mouse_button_down = false;
// An exit_pointerlock has been asked for and its change event is pending.
static bool pointer_lock_releasing = false;

// True while the machine executes: only then can the guest read the
// pointer's deltas and the keys, so only then is a grab of either useful.
static bool machine_running(void) {
    scheduler_t *s = system_scheduler();
    return s && scheduler_is_running(s);
}

// The host keys held down, and the keys they were delivered as (host_keys.c:
// one path for every machine, by ADB raw keycode).
static host_keys_t g_host_keys;

// Where host key transitions go: the machine's own keyboard, through its
// substrate (a Mac's ADB or M0110A, a Lisa's COPS).  0 = taken, <0 = refused.
static int host_key_sink(int adb_code, bool down) {
    return system_input_key(adb_code, down) < 0 ? -1 : 0;
}

// Focus or pointer lock lost: the key-ups (and the button-up) will land
// elsewhere, so let go of everything now -- except Caps Lock, which the web
// frontend owns (app/web2 lib/capslock.ts).
static void host_release_all(void) {
    host_keys_release_all(&g_host_keys, host_key_sink);
    if (mouse_button_down) {
        mouse_button_down = false;
        system_input_mouse_button(false, "relative");
    }
}

// Forward declarations for input callbacks
static void setup_pointer_lock(void);
static EM_BOOL mouse_down_cb(int, const EmscriptenMouseEvent *, void *);
static EM_BOOL mouse_up_cb(int, const EmscriptenMouseEvent *, void *);
static EM_BOOL plock_change_cb(int, const EmscriptenPointerlockChangeEvent *, void *);
static EM_BOOL mouse_move_cb(int, const EmscriptenMouseEvent *, void *);
static EM_BOOL key_down_cb(int, const EmscriptenKeyboardEvent *, void *);
static EM_BOOL key_up_cb(int, const EmscriptenKeyboardEvent *, void *);

// Pointer-lock deltas, on every machine through the substrate's relative
// operation (the Mac's hardware deltas, the Lisa's COPS reports).  This used
// to call the Mac-only system_mouse_update and special-case the Lisa by model
// id.
static void emulator_mouse_move(int dx, int dy) {
    if (!pointer_locked || (!dx && !dy))
        return;
    system_input_mouse_move(dx, dy, "relative");
}

// Mouse button down callback
static EM_BOOL mouse_down_cb(int type, const EmscriptenMouseEvent *e, void *ud) {
    (void)type;
    (void)ud;
    if (!pointer_locked) {
        // A paused or stopped machine reads no deltas: a grab would capture
        // the host pointer into a guest whose pointer cannot move.
        if (machine_running())
            emscripten_request_pointerlock("#screen", EM_FALSE);
        return EM_TRUE;
    }
    mouse_button_down = true;
    system_input_mouse_button(true, "relative");
    emulator_mouse_move(e->movementX, e->movementY);
    return EM_TRUE;
}

// Mouse button up callback
static EM_BOOL mouse_up_cb(int type, const EmscriptenMouseEvent *e, void *ud) {
    (void)type;
    (void)ud;
    if (mouse_button_down) {
        mouse_button_down = false;
        system_input_mouse_button(false, "relative");
    }
    emulator_mouse_move(e->movementX, e->movementY);
    return EM_TRUE;
}

// Pointer lock change callback
static EM_BOOL plock_change_cb(int type, const EmscriptenPointerlockChangeEvent *e, void *ud) {
    (void)type;
    (void)ud;
    pointer_locked = e->isActive;
    pointer_lock_releasing = false;
    if (!e->isActive)
        host_release_all(); // lock lost (Esc, a browser dialog stealing focus) → strand nothing
    return EM_TRUE;
}

// Window blur: the page lost focus (tab switch, OS accelerator, alt-tab), so any
// key-up will land elsewhere.  Release everything we're holding down.
static EM_BOOL blur_cb(int type, const EmscriptenFocusEvent *e, void *ud) {
    (void)type;
    (void)e;
    (void)ud;
    host_release_all();
    return EM_FALSE; // observe only; don't consume the blur
}

// Mouse move callback
static EM_BOOL mouse_move_cb(int type, const EmscriptenMouseEvent *e, void *ud) {
    (void)type;
    (void)ud;
    if (!pointer_locked)
        return EM_FALSE;
    emulator_mouse_move(e->movementX, e->movementY);
    return EM_TRUE;
}

// ============================================================================
// Keyboard: one path for every machine
// ============================================================================
//
// DOM code -> ADB raw keycode (host_keys.c) -> the machine's substrate.  There
// used to be a second table, DOM -> Lisa COPS bytes, chosen by model id, and
// only that path kept a held-key set, so a Mac whose key-up the browser kept
// (an accelerator, a focus change) was left with the key down.

// Key down callback
static EM_BOOL key_down_cb(int type, const EmscriptenKeyboardEvent *e, void *ud) {
    (void)type;
    (void)ud;
    // Keyboard goes to the machine only while the screen has grabbed input
    // (pointer lock) -- otherwise keys belong to the web2 UI (config dialog,
    // debug fields, the gs terminal).  Gate on the C-side pointer_locked flag,
    // NOT on document.activeElement: these html5 input callbacks run on the
    // emscripten worker thread (PROXY_TO_PTHREAD), where `document` is
    // undefined.
    if (!pointer_locked)
        return EM_FALSE;
    int k = host_keymap_dom_to_adb(e->code);
    // Caps Lock (0x39) is a mechanically LOCKING key, and only the DOM layer
    // can see the host's true caps state (getModifierState) -- the emscripten
    // C callback cannot.  So the C side never touches it: the event is left
    // unconsumed for the web2 frontend, which mirrors the host caps state into
    // the guest latch and the ⇪ indicator, and re-asserts it across
    // boot/restart (app/web2 lib/capslock.ts).
    if (k < 0 || k == 0x39)
        return EM_FALSE;
    // A key the machine refuses is left to the browser -- except Control,
    // which host_keys retries as Command where the keyboard has no Control
    // (the Lisa: SCO Xenix reads Apple-D as Control-D), so Control-D is not
    // left for the browser to take as "bookmark".
    return host_keys_down(&g_host_keys, k, host_key_sink) ? EM_TRUE : EM_FALSE;
}

// Key up callback
static EM_BOOL key_up_cb(int type, const EmscriptenKeyboardEvent *e, void *ud) {
    (void)type;
    (void)ud;
    // Always deliver the up for a key we sent down -- even if focus moved to a
    // web2 field meanwhile, or pointer lock was lost -- otherwise the key sticks
    // down and the guest typematic-repeats it forever.
    int k = host_keymap_dom_to_adb(e->code);
    if (k < 0 || k == 0x39)
        return EM_FALSE;
    return host_keys_up(&g_host_keys, k, host_key_sink) ? EM_TRUE : EM_FALSE;
}

// Setup pointer lock callbacks
static void setup_pointer_lock(void) {
    emscripten_set_mousedown_callback("#screen", NULL, EM_TRUE, mouse_down_cb);
    emscripten_set_mouseup_callback("#screen", NULL, EM_TRUE, mouse_up_cb);
    emscripten_set_pointerlockchange_callback(EMSCRIPTEN_EVENT_TARGET_DOCUMENT, NULL, EM_TRUE, plock_change_cb);
    emscripten_set_mousemove_callback(EMSCRIPTEN_EVENT_TARGET_DOCUMENT, NULL, EM_TRUE, mouse_move_cb);
    emscripten_set_keydown_callback(EMSCRIPTEN_EVENT_TARGET_DOCUMENT, NULL, EM_TRUE, key_down_cb);
    emscripten_set_keyup_callback(EMSCRIPTEN_EVENT_TARGET_DOCUMENT, NULL, EM_TRUE, key_up_cb);
    // Release any held keys if the page loses focus, so a key-up that lands
    // elsewhere (browser accelerator, tab switch) can't strand a key down.
    emscripten_set_blur_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, NULL, EM_TRUE, blur_cb);
}

// ============================================================================
// Main Loop Timing and Execution
// ============================================================================

#define PERF_UPDATE_INTERVAL 60 // Calculate performance every X ticks
#define CHECKPOINT_INTERVAL  900 // Background checkpoint every 900 ticks (~15 seconds at 60 ticks/sec)

// Forward declaration

// Global state variables
static int tick_counter = 0;
static int checkpoint_tick_counter = 0;
static bool checkpoint_auto_enabled = true; // Can be disabled for tests
static double last_time = 0;
// The page's pacing (the toolbar, ?speed=, scheduler.mode / speed /
// max_speed): host state, so it outlives every machine, is never in a
// checkpoint, and reaches the machine only as the run loop's argument.
static host_pacing_t s_pacing = HOST_PACING_DEFAULT;

host_pacing_t *platform_pacing(void) {
    return &s_pacing;
}

// The instruction count at the last perf sample (MIPS is the delta).
static uint64_t last_instr = 0;
// The activity lights' state and the counter baselines they compare against.
static drive_activity_t lights;
static double ticks_per_second = 0;

// Per-tick wall-clock samples for the last PERF_UPDATE_INTERVAL ticks: the
// whole em_main_tick, and the part of it spent inside shell_poll serving a
// bridge request.  The MIPS/ticks-per-second figures above are rates
// averaged over the window and cannot show a single long tick -- a 300 ms
// checkpoint or a slow bridge request inside an otherwise 60 Hz window --
// which is exactly what a frame stutter is.  Pushed with the perf update as
// the window's max and median (microseconds) so the status bar can show
// them and a spec can assert on them.
static double tick_wall_ms[PERF_UPDATE_INTERVAL];
static double tick_poll_ms[PERF_UPDATE_INTERVAL];
static double tick_poll_ms_current; // set by em_main_tick, read by tick()

// max and median of `n` samples, without disturbing the ring
static void perf_window_stats(const double *samples, int n, double *max_out, double *p50_out) {
    double sorted[PERF_UPDATE_INTERVAL];
    memcpy(sorted, samples, sizeof(double) * n);
    // insertion sort: n is 60
    for (int i = 1; i < n; i++) {
        double v = sorted[i];
        int j = i - 1;
        while (j >= 0 && sorted[j] > v) {
            sorted[j + 1] = sorted[j];
            j--;
        }
        sorted[j + 1] = v;
    }
    *max_out = sorted[n - 1];
    *p50_out = sorted[n / 2];
}

// ============================================================================
// The mailbox (every JS -> C request)
// ============================================================================
//
// THREADING MODEL — read this before changing anything in this section.
//
// We build with -sPROXY_TO_PTHREAD, which spawns a worker pthread and runs
// `main()` (and therefore `shell_init()`, `system_create()`,
// `emscripten_set_main_loop(em_main_tick, ...)`) on that worker. The worker
// owns every piece of emulator state: scheduler, machine, devices, RAM,
// OPFS file handles. The JS main thread keeps its own Module instance for
// canvas + DOM + xterm, but it does NOT own emulator state.
//
// IMPORTANT: with PROXY_TO_PTHREAD, exported Wasm functions are ALSO
// callable directly from the main JS thread via `Module.ccall(...)`. Such
// a call does NOT proxy to the worker — it executes the Wasm code on the
// main thread, with the main thread's pthread context, while the worker
// is concurrently running `em_main_tick`. Only functions that Emscripten
// emits into `proxiedFunctionTable` (a small set of built-in callbacks
// like pointerlock / mouse / visibility) get auto-proxied. None of our
// `_em_*` exports are in that table.
//
// Calling shell-touching code from the main thread is therefore unsafe:
//   - It races the worker for scheduler / machine / device state
//   - WASMFS / OPFS handles opened on the worker pthread are not
//     guaranteed to behave correctly from another thread
//   - Mutexes inside the runtime can deadlock or stall for many seconds
//
// Real-world fallout from violating this rule (a regression, 2026-05-02):
// `Module.ccall('em_gs_eval', ...)` was used for the typed object-model
// bridge (`gsEval` / `gsInspect`) and ran shell_dispatch() on the main
// thread.  E2E tests using checkpoint --save / --load via gsEval saw
// 60–90 s per call, post-load `run` not advancing the emulator, and
// "browser closed" crashes. Probes (pthread_self() inside shell_poll vs.
// inside em_gs_eval) confirmed two distinct thread IDs.
//
// THE RULE
// --------
// JS -> C must always go through the mailbox below: the page writes a
// request record into the request ring and wakes the worker; the worker's
// `shell_poll()` (called from `em_main_tick`, and from the idle wait on a
// stopped machine) drains the ring and writes each result into the event
// ring.  ccall on `_em_*` exports is forbidden -- and no longer possible:
// the Makefile stopped exporting ccall/cwrap, so only the mailbox remains.
//
// The region is static so its address is fixed for the process lifetime
// (shared memory grows in place, em_audio.c).  It is laid out by a
// constructor, before main() and before the page can see it, so the MAGIC
// and VERSION words are valid from the first read.  READY stays 0 until
// main() has run shell_init/setup_init.
static gs_mailbox_t g_mailbox;

// A GPU wait on the emulator thread (system.h, gs_v2gpu_keepalive): the
// core is busy, not dead, so the page's stall watch must not fire.
void gs_v2gpu_keepalive(void) {
    gs_mailbox_heartbeat(&g_mailbox);
}
static uint8_t g_mailbox_region[GS_MBX_ALIGN + GS_MBX_CTRL_WORDS * 4u + GS_MBX_REQ_BYTES + GS_MBX_EVT_BYTES];

__attribute__((constructor)) static void mailbox_construct(void) {
    if (!gs_mailbox_init(&g_mailbox, g_mailbox_region, GS_MBX_REQ_BYTES, GS_MBX_EVT_BYTES, gs_eval))
        abort();
    // Answers to the page carry what the leaf printed (gs_out.h); the
    // terminal shows it with the result.
    gs_mailbox_set_capture_output(&g_mailbox, true);
}

EMSCRIPTEN_KEEPALIVE uint32_t *get_gs_mailbox(void) {
    return (uint32_t *)g_mailbox.ctrl;
}

// Core events (gs_event.h) go out as records on the event ring.
void gs_event_emit(gs_event_kind_t kind, const char *json) {
    uint32_t k = kind == GS_EVENT_STATE ? GS_MBX_EVT_STATE : kind == GS_EVENT_LOG ? GS_MBX_EVT_LOG : GS_MBX_EVT_NOTIFY;
    gs_mailbox_emit(&g_mailbox, k, json);
}

uint32_t gs_current_client(void) {
    return gs_mailbox_current_client(&g_mailbox);
}

// A bare `scheduler.run` typed in the browser's terminal returns at once
// and the machine runs on (Ctrl-C stops it); only a budgeted run holds
// the line.  The script suites' "run until stopped" is headless's.
bool job_glue_unbounded_waits(uint32_t client) {
    (void)client;
    return false;
}

// The page parks in Atomics.waitAsync on READY and EVT_HEAD.
void gs_mailbox_notify(volatile uint32_t *word) {
    emscripten_atomic_notify((void *)word, INT_MAX);
}

static double mailbox_now_us(void) {
    return emscripten_get_now() * 1000.0;
}

// Host microseconds one drain may spend serving a burst of requests before
// giving the tick back; a leaf that is running when it expires still
// completes (leaves are bounded by their data, not by this).  Set from
// the tick instrumentation once it has been measured.
#define GS_MAILBOX_DRAIN_US 2000.0

// On a stopped machine the tick parks here between frames so a request is
// served at once instead of at the next RAF.  Bounded, in slices, with the
// pthread's proxied tasks (keyboard, mouse, pointer lock: all delivered as
// tasks to this thread) drained between slices -- a plain futex wait would
// hold them until the next request arrived.  Returns to the event loop
// after GS_MAILBOX_IDLE_MS whatever happened.
#define GS_MAILBOX_IDLE_SLICE_MS 4.0
#define GS_MAILBOX_IDLE_MS       12.0

int shell_poll(void) {
    int n = gs_mailbox_drain(&g_mailbox, GS_MAILBOX_DRAIN_US, mailbox_now_us);
    if (n > 0)
        gs_mailbox_notify(&g_mailbox.ctrl[GS_MBX_C_EVT_HEAD]);
    return n;
}

// The idle wait of a stopped machine: serve requests as they arrive for up
// to GS_MAILBOX_IDLE_MS, then return to the event loop.  Returns the
// number of results written.
static int mailbox_idle_wait(void) {
    double t0 = emscripten_get_now();
    int served = 0;
    while (emscripten_get_now() - t0 < GS_MAILBOX_IDLE_MS) {
        if (gs_mailbox_has_requests(&g_mailbox)) {
            served += shell_poll();
            continue;
        }
        uint32_t seen = mbx_load(g_mailbox.ctrl, GS_MBX_C_REQ_HEAD);
        emscripten_futex_wait(&g_mailbox.ctrl[GS_MBX_C_REQ_HEAD], seen, GS_MAILBOX_IDLE_SLICE_MS);
        emscripten_current_thread_process_queued_calls();
        gs_mailbox_heartbeat(&g_mailbox);
    }
    return served;
}

// Main tick function called by the Emscripten main loop
void em_main_tick(void) {
    tick_counter++;

    // Calculate performance metrics every PERF_UPDATE_INTERVAL ticks and
    // push them to the UI (MIPS from instr_count deltas —
    // without this, nothing in web2 would reveal a throughput regression).
    if (tick_counter % PERF_UPDATE_INTERVAL == 0) {
        double current_time = emscripten_get_now();
        uint64_t instr_now = cpu_instr_count();

        if (last_time > 0) {
            double elapsed_ms = current_time - last_time;
            ticks_per_second = (PERF_UPDATE_INTERVAL * 1000.0) / elapsed_ms;
            double mips = (double)(instr_now - last_instr) / (elapsed_ms * 1000.0);
            double tick_max, tick_p50, poll_max, poll_p50;
            perf_window_stats(tick_wall_ms, PERF_UPDATE_INTERVAL, &tick_max, &tick_p50);
            perf_window_stats(tick_poll_ms, PERF_UPDATE_INTERVAL, &poll_max, &poll_p50);
            gs_event_emitf(GS_EVENT_STATE,
                           "{\"event\":\"perf\",\"mips\":%.2f,\"tps\":%.1f,\"tick_max_ms\":%.3f,\"tick_p50_ms\":%.3f,"
                           "\"poll_max_ms\":%.3f}",
                           mips, ticks_per_second, tick_max, tick_p50, poll_max);
        }

        last_time = current_time;
        last_instr = instr_now;
    }

    // Execute based on emulation state
    scheduler_t *sched = system_scheduler();
    if (sched && scheduler_is_running(sched)) {
        double now = emscripten_get_now(); // milliseconds

        // Trigger background checkpoint every CHECKPOINT_INTERVAL ticks while running (if enabled)
        if (checkpoint_auto_enabled) {
            checkpoint_tick_counter++;
            if (checkpoint_tick_counter >= CHECKPOINT_INTERVAL) {
                checkpoint_tick_counter = 0;
                system_quick_checkpoint("tick-auto", false, true);
            }
        }

        scheduler_main_loop(global_emulator, now, &s_pacing); // Pass milliseconds

        // Update video if framebuffer changed
        em_video_update();
    }

    // Poll for pending shell commands every tick.  This must run regardless
    // of running state so that drag-and-drop media inserts, checkpoint
    // commands, etc. execute while the emulator is running.
    //
    // While paused nothing above repaints, yet a served request can change
    // what is on screen: a debug.step, a memory.poke into the framebuffer, a
    // CLUT write, a media change.  So repaint after a served request -- and
    // only then: an idle paused tick costs nothing, and em_video_update
    // compares before uploading, so a request that changed nothing uploads
    // nothing.
    // Re-fetch the scheduler: the request may have booted or restarted the
    // machine, freeing the one fetched above.
    gs_mailbox_heartbeat(&g_mailbox);
    double poll_t0 = emscripten_get_now();
    int served = shell_poll();
    // A stopped machine has nothing to do until the next request: wait for
    // one here (bounded) instead of at the next RAF.
    if (!(sched && scheduler_is_running(sched)))
        served += mailbox_idle_wait();
    tick_poll_ms_current = emscripten_get_now() - poll_t0;
    if (served) {
        scheduler_t *after = system_scheduler();
        if (!(after && scheduler_is_running(after)))
            em_video_update();
    }

    // The machine stopped (pause, breakpoint, the end of a step) while the
    // screen held the pointer: hand the pointer and the keys back to the page.
    if (pointer_locked && !pointer_lock_releasing && !machine_running()) {
        pointer_lock_releasing = true;
        emscripten_exit_pointerlock();
    }

    // The run state and the floppy drives are the core's to announce now
    // (mode_started / mode_ended from the scheduler, floppy from the
    // controller): nothing is diffed here any more.

    // The HD / FD / CD activity lights (a drive_activity event) on a
    // state edge only: the counters are sampled here, once per tick, and
    // drive_activity_update holds a light on for its minimum visible time.
    {
        uint64_t reads[DRIVE_KIND_COUNT], writes[DRIVE_KIND_COUNT];
        system_drive_io_counts(reads, writes);
        unsigned changed = drive_activity_update(&lights, reads, writes, emscripten_get_now());
        for (int k = 0; k < DRIVE_KIND_COUNT; k++) {
            if (!(changed & (1u << k)))
                continue;
            gs_event_emitf(GS_EVENT_NOTIFY, "{\"event\":\"drive_activity\",\"kind\":%d,\"state\":%d}", k,
                           (int)lights.light[k]);
        }
    }
}

// A new machine is the active one (platform hook, system.h).  Nothing sampled
// from the previous machine is compared with this one: the MIPS meter skips
// the sample that would straddle the change, and the activity lights go dark
// and take their baselines from the new machine's counters.
void platform_machine_attached(void) {
    last_time = 0; // the next perf sample only sets the baseline
    last_instr = cpu_instr_count();
    for (int k = 0; k < DRIVE_KIND_COUNT; k++) {
        if (lights.light[k] != DRIVE_LIGHT_IDLE)
            gs_event_emitf(GS_EVENT_NOTIFY, "{\"event\":\"drive_activity\",\"kind\":%d,\"state\":%d}", k,
                           (int)DRIVE_LIGHT_IDLE);
    }
    memset(&lights, 0, sizeof(lights));
    em_video_machine_attached();
}

// Exposed tick wrapper for Emscripten main loop.  Times the whole tick and
// records it, with the shell_poll share em_main_tick measured, into the
// perf window (see tick_wall_ms).
void tick(void) {
    double t0 = emscripten_get_now();
    tick_poll_ms_current = 0;
    em_main_tick();
    int slot = tick_counter % PERF_UPDATE_INTERVAL;
    tick_wall_ms[slot] = emscripten_get_now() - t0;
    tick_poll_ms[slot] = tick_poll_ms_current;
}

// Forward formatted log lines to the page as log events.
// Installed once at boot via log_set_sink so the new-UI Logs view can
// fan emissions out to a per-category mirror without inferring them
// from Module.print (which captures everything, not just LOG sites).
static void js_log_sink(const char *line, void *user) {
    (void)user;
    if (!line)
        return;
    gs_event_emit_text(GS_EVENT_LOG, "log", "line", line);
}

// SIGINT handler — stops the scheduler so a real Ctrl-C in the headless
// driver, or any other process-level signal, halts emulation cleanly.
// JS pauses the emulator via gsEval('scheduler.stop'), which routes
// through the object-model channel like every other JS→C call.
void sigint_handler(int sig) {
    (void)sig;
    scheduler_t *sched = system_scheduler();
    if (sched)
        scheduler_stop(sched);
}

// ============================================================================
// Filesystem Commands
// ============================================================================

// ============================================================================
// LaserWriter interpreter worker (the ring transport's platform hooks)
// ============================================================================
// The printer bridge runs its PostScript interpreter in the page's platen
// worker (app/web2/src/printer/), reached through a shared-memory ring
// (laserwriter_ring_protocol.h).  The finished PDF never enters the core
// here: the worker posts it to the page, which downloads it — so there is
// no laserwriter_sink_document override on this platform (the weak default
// in laserwriter_job.c is never reached: the ring result carries no bytes).

// The bridge allocated its control block at `ctrl_addr` (emulation
// pthread): ask the page to start the worker and attach it, the way
// em_gpu.c reaches Module.onVoodooGpuAttach.  The library version names
// the module file the page fetches (platen-<version>.js, laserwriter.mk).
void laserwriter_ring_attach_requested(uintptr_t ctrl_addr) {
    static const char version[] = GS_PLATEN_VERSION;
    // clang-format off
    MAIN_THREAD_ASYNC_EM_ASM(
        { if (typeof Module.onPrinterAttach === 'function') Module.onPrinterAttach($0, UTF8ToString($1)); },
        (uint32_t)ctrl_addr, version);
    // clang-format on
}

// Wakes the worker parked in Atomics.waitAsync on a control word.
void laserwriter_ring_notify(volatile uint32_t *addr) {
    emscripten_futex_wake(addr, INT_MAX);
}

// ============================================================================
// Downloads: a file (or a buffer) to the page, through a transfer buffer
// ============================================================================
// An I/O job (io/io_worker.h) reads the file GS_DL_CHUNK bytes at a time
// into a buffer the page can see; each chunk is announced to the page as
// EVT_NOTIFY {"event":"download_chunk","id":req,"handle":h,"ptr":p,
// "len":n,"last":0|1,"name":...} (a note the emulator thread turns into the
// event: mailbox.h, transfer buffers), the page copies the bytes into a Blob
// part and answers REQ_ACK_BUF {handle}, and the worker refills.  Neither
// the emulator thread nor the page ever waits on the other; a page that
// never acks times the JOB out (GS_DL_ACK_MS), not the machine.  This
// replaced a whole-file malloc + fread and a MAIN_THREAD_EM_ASM that
// blocked the reading thread until the page had copied everything.
#define GS_DL_CHUNK  (4u << 20)
#define GS_DL_ACK_MS 30000u

typedef struct {
    char *path; // the file to read, or NULL for `bytes`
    uint8_t *bytes; // an in-memory source (copied), or NULL
    size_t bytes_len;
    char name[256]; // the download's file name
    char meta[384]; // extra JSON members for the page (",\"kind\":\"document\",..."), or ""
    uint32_t token; // the deferral (0: answering now)
    uint32_t io_id; // the worker job
    uint32_t handle; // the transfer buffer (published on the first chunk)
    uint8_t *buf; // GS_DL_CHUNK bytes
    uint32_t req_id; // the request, named in the events
    uint64_t total;
} download_job_t;

// The worker: fill the buffer, announce, wait for the page, repeat.
static int download_work(void *ud, char *err, size_t err_cap) {
    download_job_t *d = (download_job_t *)ud;
    FILE *f = NULL;
    if (d->path) {
        struct stat st;
        if (stat(d->path, &st) != 0) {
            snprintf(err, err_cap, "cannot access '%s': %s", d->path, strerror(errno));
            return -errno;
        }
        if (!S_ISREG(st.st_mode)) {
            snprintf(err, err_cap, "'%s' is not a regular file", d->path);
            return -EINVAL;
        }
        f = fopen(d->path, "rb");
        if (!f) {
            snprintf(err, err_cap, "cannot open '%s': %s", d->path, strerror(errno));
            return -errno;
        }
        d->total = (uint64_t)st.st_size;
    } else {
        d->total = d->bytes_len;
    }
    uint64_t sent = 0;
    for (;;) {
        if (io_check_cancelled()) {
            if (f)
                fclose(f);
            snprintf(err, err_cap, "cancelled");
            return -ECANCELED;
        }
        size_t n;
        if (f) {
            n = fread(d->buf, 1, GS_DL_CHUNK, f);
            if (n < GS_DL_CHUNK && ferror(f)) {
                fclose(f);
                snprintf(err, err_cap, "read error on '%s'", d->path);
                return -EIO;
            }
        } else {
            n = d->bytes_len - (size_t)sent;
            if (n > GS_DL_CHUNK)
                n = GS_DL_CHUNK;
            memcpy(d->buf, d->bytes + sent, n);
        }
        sent += n;
        bool last = sent >= d->total;
        char note[IO_NOTE_MAX];
        snprintf(note, sizeof note, "{\"chunk\":%u,\"last\":%d}", (unsigned)n, last ? 1 : 0);
        io_note(note);
        int rc = io_wait_ack(0, GS_DL_ACK_MS);
        if (rc != 0) {
            if (f)
                fclose(f);
            snprintf(err, err_cap, rc == -ECANCELED ? "cancelled" : "the page did not take the download");
            return rc;
        }
        io_report_progress(sent, d->total);
        if (last)
            break;
    }
    if (f)
        fclose(f);
    return 0;
}

// The emulator thread, per chunk: publish the buffer once, tell the page.
static void download_note(const char *json, void *ud) {
    download_job_t *d = (download_job_t *)ud;
    unsigned n = 0;
    int last = 0;
    sscanf(json, "{\"chunk\":%u,\"last\":%d}", &n, &last);
    if (!d->handle)
        d->handle = gs_transfer_publish(d->io_id);
    if (!d->handle) {
        // No room in the transfer table: the job times out on its ack.
        printf("download: no transfer buffer for '%s'\n", d->name);
        return;
    }
    gs_event_emitf(GS_EVENT_NOTIFY,
                   "{\"event\":\"download_chunk\",\"id\":%u,\"handle\":%u,\"ptr\":%u,\"len\":%u,\"last\":%d,"
                   "\"name\":\"%s\"%s}",
                   (unsigned)d->req_id, (unsigned)d->handle, (unsigned)(uintptr_t)d->buf, n, last, d->name, d->meta);
}

static void download_progress(uint64_t done, uint64_t total, void *ud) {
    download_job_t *d = (download_job_t *)ud;
    if (d->token)
        gs_result_progress(d->token, done, total);
}

static void download_free(download_job_t *d) {
    if (d->handle)
        gs_transfer_release(d->handle);
    free(d->buf);
    free(d->bytes);
    free(d->path);
    free(d);
}

static void download_done(bool ok, double ms, const char *error, void *ud) {
    (void)ms;
    download_job_t *d = (download_job_t *)ud;
    if (ok) {
        printf("download: requested '%s'\n", d->name);
        if (d->token)
            gs_result_complete_ok(d->token);
    } else {
        printf("download: %s\n", error ? error : "failed");
        if (d->token)
            gs_result_complete_error(d->token, error ? error : "download failed");
    }
    download_free(d);
}

// Starts the download job; consumes `d`.  0 when the job runs (the
// deferral answers), else -1 (no worker: a chunk can never be acked while
// this thread holds the buffer).
static int download_start(download_job_t *d) {
    d->buf = (uint8_t *)malloc(GS_DL_CHUNK);
    if (!d->buf) {
        printf("download: out of memory\n");
        download_free(d);
        return -1;
    }
    d->token = gs_result_defer();
    d->req_id = gs_result_request_id(d->token);
    io_job_desc_t desc = {
        .work = download_work,
        .work_ud = d,
        .done = download_done,
        .done_ud = d,
        .progress = download_progress,
        .note = download_note,
        .observer_ud = d,
    };
    d->io_id = io_submit_job(&desc);
    if (d->io_id) {
        if (d->token)
            gs_result_bind_io(d->token, d->io_id);
        return 0;
    }
    printf("download: the I/O worker is not running; cannot hand '%s' to the page\n", d->name);
    if (d->token)
        gs_result_complete_error(d->token, "the I/O worker is not running");
    download_free(d);
    return -1;
}

// Platform sink for a job's captured PostScript (appletalk.printer.capture):
// downloaded as <job>.ps, the way the page downloads the job's PDF.
void laserwriter_sink_capture(const laserwriter_capture_t *cap) {
    download_job_t *d = (download_job_t *)calloc(1, sizeof(*d));
    if (!d)
        return;
    snprintf(d->name, sizeof d->name, "%05u.ps", (unsigned)cap->job_id);
    d->bytes = (uint8_t *)malloc(cap->ps_len ? cap->ps_len : 1);
    if (!d->bytes) {
        free(d);
        return;
    }
    memcpy(d->bytes, cap->ps, cap->ps_len);
    d->bytes_len = cap->ps_len;
    download_start(d);
}

// Copy `s` into `out` with what JSON strings and file names cannot hold
// replaced: quotes, backslashes and control bytes become '_' (`file`: also
// anything outside [A-Za-z0-9.-]).
static void sanitize(char *out, size_t cap, const char *s, bool file) {
    size_t n = 0;
    for (; s && *s && n + 1 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        bool ok = c >= 0x20 && c < 0x7F && c != '"' && c != '\\';
        if (file)
            ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '.' || c == '-';
        out[n++] = ok ? (char)c : '_';
    }
    out[n] = '\0';
}

// Platform sink for a document a printer rasterised in the core (the
// ImageWriter, printer_sink.h): the bytes go to the page through the
// download path, marked as a document, and the page shows them in the print
// viewer (or downloads them where the browser has no PDF viewer).
void printer_sink_document(const printer_document_t *doc) {
    download_job_t *d = (download_job_t *)calloc(1, sizeof(*d));
    if (!d)
        return;
    char title[PRINTER_TITLE_MAX + 1], ftitle[PRINTER_TITLE_MAX + 1], printer[64];
    sanitize(title, sizeof title, doc->title, false);
    sanitize(ftitle, sizeof ftitle, doc->title, true);
    sanitize(printer, sizeof printer, doc->printer, false);
    snprintf(d->name, sizeof d->name, "%s-%05u-%s.pdf", doc->slug, (unsigned)doc->job_id,
             ftitle[0] ? ftitle : "untitled");
    snprintf(d->meta, sizeof d->meta,
             ",\"kind\":\"document\",\"printer\":\"%s\",\"job\":%u,\"pages\":%u,\"title\":\"%s\"", printer,
             (unsigned)doc->job_id, (unsigned)doc->pages, title);
    d->bytes = (uint8_t *)malloc(doc->pdf_len ? doc->pdf_len : 1);
    if (!d->bytes) {
        free(d);
        return;
    }
    memcpy(d->bytes, doc->pdf, doc->pdf_len);
    d->bytes_len = doc->pdf_len;
    download_start(d);
}

// Platform sink for a printer job's raw input (the printer's `capture`):
// downloaded as <printer>-<job>.<ext>.
void printer_sink_capture(const printer_capture_t *cap) {
    download_job_t *d = (download_job_t *)calloc(1, sizeof(*d));
    if (!d)
        return;
    snprintf(d->name, sizeof d->name, "%s-%05u.%s", cap->slug, (unsigned)cap->job_id, cap->ext);
    d->bytes = (uint8_t *)malloc(cap->len ? cap->len : 1);
    if (!d->bytes) {
        free(d);
        return;
    }
    memcpy(d->bytes, cap->data, cap->len);
    d->bytes_len = cap->len;
    download_start(d);
}

// A core printer's status changed: the same printer_status event the
// LaserWriter sends, naming the printer.
void printer_sink_status(const char *printer, const char *status) {
    char p[64], st[128];
    sanitize(p, sizeof p, printer, false);
    sanitize(st, sizeof st, status, false);
    gs_event_emitf(GS_EVENT_NOTIFY, "{\"event\":\"printer_status\",\"printer\":\"%s\",\"status\":\"%s\"}", p, st);
}

// Platform impl of gs_download (weak default in system.c stubs out).
// Returns 0 when the download was started (the answer is deferred: it
// completes when the page has taken the last chunk), non-zero otherwise.
int gs_download(const char *path) {
    download_job_t *d = (download_job_t *)calloc(1, sizeof(*d));
    if (!d)
        return -1;
    d->path = strdup(path);
    if (!d->path) {
        free(d);
        return -1;
    }
    const char *name = strrchr(path, '/');
    snprintf(d->name, sizeof d->name, "%s", name ? name + 1 : path);
    return download_start(d);
}

// ============================================================================
// Background Checkpoint System
// ============================================================================
// Per-machine layout (docs/guide/ARCHITECTURE.md):
//   /opfs/checkpoints/<machine_id>-<created>/state.checkpoint      (current)
//   /opfs/checkpoints/<machine_id>-<created>/state.checkpoint.tmp  (in-flight)
// One file per machine; tmp+rename is the atomic swap.

static bool g_background_handlers_installed = false;

// Request background checkpoint (with rate limiting).  Honours checkpoint.auto
// like the tick loop does: with automatic saving off, hiding the tab must not
// write a checkpoint either (#148).  Explicit checkpoint.save is unaffected.
static void maybe_request_background_checkpoint(const char *reason, bool rate_limit) {
    if (!checkpoint_auto_enabled)
        return;
    int rc = system_quick_checkpoint(reason, false, rate_limit);
    if (rc != GS_SUCCESS) {
        printf("[checkpoint] background checkpoint failed (%s)\n", reason ? reason : "background");
    }
}

// Visibility change callback
static EM_BOOL background_visibility_callback(int eventType, const EmscriptenVisibilityChangeEvent *event,
                                              void *userData) {
    (void)eventType;
    if (!event || !event->hidden)
        return EM_FALSE;
    maybe_request_background_checkpoint((const char *)userData, true);
    return EM_FALSE;
}

// Install background checkpoint handlers.  Only visibilitychange: it is
// delivered to this (the emulator) thread, and browsers fire it -> hidden when
// a tab is closed or navigated away.  That save is asynchronous, so on an
// unload it may not finish before the page goes: a reload does not reliably
// find a checkpoint from it.  There is deliberately no beforeunload handler:
// Emscripten runs that callback on the browser main thread (it must return
// synchronously), which put a whole checkpoint -- system_checkpoint and
// WasmFS fopen/fwrite/rename -- on the main thread while the worker could be
// mid-tick.  Blocking the main thread instead, waiting for the worker, could
// stall the save itself: the worker's tick is driven by requestAnimationFrame.
static void install_background_checkpoint_handlers(void) {
    if (g_background_handlers_installed)
        return;
    emscripten_set_visibilitychange_callback((void *)"visibilitychange", EM_FALSE, background_visibility_callback);
    g_background_handlers_installed = true;
}

// The web app's scratch area (app/web2 lib/opfsPaths.ts SCRATCH_DIR): every
// file the page writes on its way somewhere else -- an upload being probed, a
// URL download, a streamed import's .dmg.part, a Save State being downloaded
// -- lives there, in its tab's own part, and the operation that wrote it
// removes it on every exit.  The page clears what no live tab holds
// (bus/scratch.ts): the core cannot tell a closed tab's part from another
// open tab's, so it only makes sure the directory exists.  The rest of
// /opfs/upload is the user's, and is not touched.
#define SCRATCH_DIR "/opfs/upload/.scratch"

// ============================================================================
// Exported Runtime Query Functions (for tests and diagnostics)
// ============================================================================

// ============================================================================
// Main Entry Point
// ============================================================================

int main(void) {
    signal(SIGINT, sigint_handler);
    debug_set_failure_hook(em_assertion_callback);

    // Single OPFS mount at /opfs — everything under it persists.
    // The root stays memory-backed (wasmfs_create_opfs_backend() cannot run
    // during global constructors on the main thread).
    // The web app creates its directory structure under /opfs; users can also
    // create arbitrary paths under /opfs for their own persistent storage.
    backend_t opfs = wasmfs_create_opfs_backend();
    wasmfs_create_directory("/opfs", 0777, opfs);

    // Web app directory structure (regular mkdir inside the OPFS mount).
    mkdir("/opfs/images", 0777);
    mkdir("/opfs/images/rom", 0777);
    mkdir("/opfs/images/vrom", 0777);
    mkdir("/opfs/images/prom", 0777);
    mkdir("/opfs/images/fd", 0777);
    mkdir("/opfs/images/fdhd", 0777);
    mkdir("/opfs/images/hd", 0777);
    mkdir("/opfs/images/cd", 0777);
    mkdir("/opfs/checkpoints", 0777);
    mkdir("/opfs/upload", 0777);
    mkdir(SCRATCH_DIR, 0777);

    // Offer every file in the persistent vROM store to the core's content-
    // addressed registry (names are irrelevant — each offer is identified by
    // content).  The platform names the directory; core walks it and never
    // builds a path of its own.  Mid-session uploads are offered by the
    // web app's ingest path (catalog.vroms.offer), so this startup pass only
    // needs to cover what already persisted.
    vrom_offer_dir("/opfs/images/vrom", NULL);
    // ...and the same pass over the persistent PCI expansion-ROM store, for
    // the same reason: without it a .prom that persisted in an earlier
    // session is invisible after a reload, and the card it drives looks
    // uninstallable until the user uploads the file again.
    prom_offer_dir("/opfs/images/prom", NULL);

    // Volatile scratch space on memory backend (visible from all threads).
    backend_t membk = wasmfs_create_memory_backend();
    wasmfs_create_directory("/tmp", 0777, membk);
    mkdir("/tmp/upload", 0777);
    mkdir("/tmp/extract", 0777);

    // The page passes no command line: a machine is made by machine.boot and
    // the pacing is scheduler.mode, both over the bridge like everything
    // else.  (--model and --speed used to be parsed here; nothing passed
    // them, and ?speed= documented as reaching --speed never did.)
    shell_init();
    setup_init();
    system_set_default_share(GS_DEFAULT_SHARE_PATH);

    // Route every log_emit onto the event ring so the new-UI Logs
    // view gets a structured stream parallel to stdout. shell_init has
    // already called log_init; setting the sink here also forwards any
    // categories registered later (setup_init, machine boot, …).
    log_set_sink(js_log_sink, NULL);

    // The mailbox is open for business. JS gates its first gsEval on
    // READY so requests issued during the boot window don't dispatch
    // against the empty default root class. The notify wakes any JS
    // thread parked in Atomics.waitAsync on that word.
    gs_mailbox_set_ready(&g_mailbox);

    // The job thread: scripts run there, not here (job/job.h).  Created
    // now, not at the first script -- pthread_create from this pthread is
    // proxied to the browser's main thread, and the Worker takes tens of
    // milliseconds to come up.  512 KB of stack covers the interpreter's
    // recursion (16 frames of functions, the expression parser).
    if (!job_thread_start(512u << 10))
        fprintf(stderr, "job thread could not be started; scripts run inline\n");
    // The I/O worker: the checkpoint's write and rename run there, not in
    // the tick (io/io_worker.h).  Same reason to create it now.
    if (!io_worker_start(256u << 10))
        fprintf(stderr, "I/O worker could not be started; writes run inline\n");

    // Initialize subsystems (safe without a machine — video and audio handle NULL)
    em_video_init();
    em_audio_init();
    em_camera_init(); // announce the webcam frame transport to the main thread
    em_audio_in_init(); // ...and the microphone sample ring
    setup_pointer_lock();

    install_background_checkpoint_handlers();

    emscripten_set_main_loop(tick, 0, 1); // Use RAF, simulate infinite loop
    return 0;
}

// ============================================================================
// Assertion Notification for JavaScript
// ============================================================================

// Platform-specific assertion callback implementation: the failure goes
// out as an event (assert_failed) the page and its tests read off the
// event ring; nothing here blocks on the browser's main thread.
static void em_assertion_callback(const char *kind, const char *expr, const char *file, int line, const char *func) {
    if (!expr)
        expr = kind;
    char where[512];
    snprintf(where, sizeof where, "%s:%d %s", file ? file : "<unknown>", line, func ? func : "<unknown>");
    gs_event_emit_text(GS_EVENT_STATE, "assert_failed", "where", where);
    gs_event_emit_text(GS_EVENT_STATE, "assert_expr", "expr", expr);
}

// Background auto-checkpoint accessors — override the weak defaults in
// system.c so the `auto_checkpoint` attribute reads/writes the live flag.
bool gs_checkpoint_auto_get(void) {
    return checkpoint_auto_enabled;
}

int gs_checkpoint_auto_set(bool enabled) {
    checkpoint_auto_enabled = enabled;
    if (!enabled)
        checkpoint_tick_counter = 0;
    return 0;
}

// ============================================================================
// Diagnostics - Host Callstack
// ============================================================================

// Print the host (Emscripten/WASM or native) callstack for debugging
void em_print_host_callstack(void) {
    printf("\n=== Host callstack ===\n");
    // Print both C and JS stacks
    char stackbuf[8192];
    int n =
        emscripten_get_callstack(EM_LOG_C_STACK | EM_LOG_JS_STACK | EM_LOG_NO_PATHS, stackbuf, (int)sizeof(stackbuf));
    if (n > 0)
        printf("%s\n", stackbuf);
    else
        printf("(unavailable)\n");
}

// Nothing to walk: the browser's card ROMs live under /opfs/images/vrom and
// /opfs/images/prom, offered at startup and on every upload (persistAs).
void platform_offer_sibling_card_roms(const char *rom_path) {
    (void)rom_path;
}

// Platform-specific callstack function (exposed to core via platform.h)
void platform_print_host_callstack(void) {
    em_print_host_callstack();
}
