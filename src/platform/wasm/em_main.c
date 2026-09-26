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
#include <dirent.h>
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
#include "prom.h"
#include "scheduler.h"
#include "shell.h"
#include "system.h"
#include "vrom.h"

// ============================================================================
// Forward Declarations
// ============================================================================

static void em_assertion_callback(const char *kind, const char *expr, const char *file, int line, const char *func);

// The always-present AppleShare volume.  The path literal lives here, in the
// platform layer, because core never fabricates or interprets a path (PR #69);
// core publishes it after every machine build (system_set_default_share).
// Under OPFS the directory — and the AppleDouble sidecars the AFP server
// writes beside each file — persist across page reloads for free.
#define GS_DEFAULT_SHARE_PATH "/opfs/shared"

// ============================================================================
// Pointer-lock and Input Handling
// ============================================================================

static volatile int pointer_locked = 0;
static bool mouse_button_down = false;

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
static uint8_t g_mailbox_region[GS_MBX_ALIGN + GS_MBX_CTRL_WORDS * 4u + GS_MBX_REQ_BYTES + GS_MBX_EVT_BYTES];

__attribute__((constructor)) static void mailbox_construct(void) {
    if (!gs_mailbox_init(&g_mailbox, g_mailbox_region, GS_MBX_REQ_BYTES, GS_MBX_EVT_BYTES, gs_eval))
        abort();
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
        static uint64_t last_instr = 0;

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

        scheduler_main_loop(global_emulator, now); // Pass milliseconds

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

    // The run state and the floppy drives are the core's to announce now
    // (mode_started / mode_ended from the scheduler, floppy from the
    // controller): nothing is diffed here any more.

    // The HD / FD / CD activity lights (a drive_activity event) on a
    // state edge only: the counters are sampled here, once per tick, and
    // drive_activity_update holds a light on for its minimum visible time.
    {
        static drive_activity_t lights;
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

// Hand `len` bytes to the page as a download named `name`.  Blocks until the
// main thread has copied them, so `buf` need only live for the call.
static void em_download_bytes(const char *name, const uint8_t *buf, size_t nread) {
    // Trigger browser download on the main thread (DOM access required).
    // The worker is blocked in MAIN_THREAD_EM_ASM, so buf is valid.
    // clang-format off
    MAIN_THREAD_EM_ASM(
        {
            try {
                var ptr = $0;
                var len = $1;
                var namePtr = $2;
                var name = UTF8ToString(namePtr) || 'download.bin';
                // Access the shared heap — try both global and Module-scoped accessors
                var heap = (typeof HEAPU8 !== 'undefined') ? HEAPU8 : Module.HEAPU8;
                var data = new Uint8Array(heap.buffer, ptr, len);
                var copy = new Uint8Array(data);  // copy out of shared buffer
                var blob = new Blob([copy], {type: 'application/octet-stream'});
                var a = document.createElement('a');
                a.href = URL.createObjectURL(blob);
                a.download = name;
                document.body.appendChild(a);
                a.click();
                document.body.removeChild(a);
                setTimeout(function() {
                    try { URL.revokeObjectURL(a.href); } catch (e) {}
                }, 0);
            } catch (e) {
                console.error('[download] MAIN_THREAD_EM_ASM failed:', e);
            }
        },
        buf, (int)nread, name);
    // clang-format on
}

// Platform sink for a job's captured PostScript (appletalk.printer.capture):
// downloaded as <job>.ps, the way the page downloads the job's PDF.
void laserwriter_sink_capture(const laserwriter_capture_t *cap) {
    char name[32];
    snprintf(name, sizeof(name), "%05u.ps", (unsigned)cap->job_id);
    em_download_bytes(name, cap->ps, cap->ps_len);
}

// Download command - save file to browser
// Platform impl of gs_download (weak default in system.c stubs out).
// Returns 0 on success, non-zero on any failure (so the typed
// `download` attribute reports the real outcome rather than always-true).
int gs_download(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        printf("download: cannot access '%s': %s\n", path, strerror(errno));
        return -1;
    }
    if (!S_ISREG(st.st_mode)) {
        printf("download: '%s' is not a regular file\n", path);
        return -1;
    }

    // Read file on the worker thread (OPFS accessible here)
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("download: cannot open '%s': %s\n", path, strerror(errno));
        return -1;
    }
    size_t file_size = (size_t)st.st_size;
    uint8_t *buf = (uint8_t *)malloc(file_size);
    if (!buf) {
        fclose(f);
        printf("download: out of memory (%zu bytes)\n", file_size);
        return -1;
    }
    size_t nread = fread(buf, 1, file_size, f);
    fclose(f);
    if (nread != file_size) {
        // Short read silently truncating the download would corrupt the
        // user's saved file.  Fail loudly instead.
        printf("download: short read on '%s' (%zu of %zu bytes)\n", path, nread, file_size);
        free(buf);
        return -1;
    }

    // Extract filename from path
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    em_download_bytes(name, buf, nread);
    free(buf);
    printf("download: requested '%s'\n", path);
    return 0;
}

// ============================================================================
// Background Checkpoint System
// ============================================================================
// Per-machine layout (docs/guide/ARCHITECTURE.md):
//   /opfs/checkpoints/<machine_id>-<created>/state.checkpoint      (current)
//   /opfs/checkpoints/<machine_id>-<created>/state.checkpoint.tmp  (in-flight)
// One file per machine; tmp+rename is the atomic swap.

static bool g_background_handlers_installed = false;

// Request background checkpoint (with rate limiting)
static void maybe_request_background_checkpoint(const char *reason, bool rate_limit) {
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

    // Offer every file in the persistent vROM store to the core's content-
    // addressed registry (names are irrelevant — each offer is identified by
    // content).  The platform names the directory; core walks it and never
    // builds a path of its own.  Mid-session uploads are offered by the
    // web app's ingest path (machine.vrom.offer), so this startup pass only
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

// Platform-specific callstack function (exposed to core via platform.h)
void platform_print_host_callstack(void) {
    em_print_host_callstack();
}
