// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// em_main.c
// Main Emscripten platform implementation - handles main loop, input, checkpointing, and filesystem commands

// ============================================================================
// Includes
// ============================================================================

#include "em.h"

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

#ifdef __EMSCRIPTEN__
#include <emscripten/stack.h>
#include <emscripten/wasmfs.h>
#else
#if defined(__linux__) || defined(__APPLE__)
#include <execinfo.h>
#endif
#endif

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
static int save_quick_checkpoint(const char *reason, bool verbose, bool rate_limit);

// Global state variables
static int tick_counter = 0;
static int checkpoint_tick_counter = 0;
static bool checkpoint_auto_enabled = true; // Can be disabled for tests
static double last_time = 0;
static double ticks_per_second = 0;

// Emscripten-specific shell stubs. Prompt composition lives in
// src/core/shell/shell.c::shell_build_prompt now (callable from JS via
// `shell.prompt` on the Shell class).
void print_prompt(void) {}

// ============================================================================
// Shared-heap Command Queue (and gs_eval queue)
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
// Real-world fallout from violating this rule (M10c regression, 2026-05-02):
// `Module.ccall('em_gs_eval', ...)` was used for the typed object-model
// bridge (`gsEval` / `gsInspect`) and ran shell_dispatch() on the main
// thread.  E2E tests using checkpoint --save / --load via gsEval saw
// 60–90 s per call, post-load `run` not advancing the emulator, and
// "browser closed" crashes. Probes (pthread_self() inside shell_poll vs.
// inside em_gs_eval) confirmed two distinct thread IDs.
//
// THE RULE
// --------
// JS → C must always go through the SAB queue below. JS writes the
// request into shared globals, sets a pending flag, and polls a done
// flag. The worker's `shell_poll()` (called from `em_main_tick`)
// drains the queue and writes the result. ccall on `_em_*` exports is
// forbidden -- and no longer possible: the Makefile stopped exporting
// ccall/cwrap (A7), so only the bridge remains.
//
// The single shared-memory region. Layout in em.h, mirrored in
// app/web2/src/bus/emulator.ts (offsets pinned by em.h's _Static_asserts).
// Path and args are fixed-size; gsEval refuses a request that would not
// fit rather than let it be truncated.  Output carries `meta.*`
// introspection dumps, which dominate.
static js_bridge_t g_bridge = {.version = JS_BRIDGE_VERSION};

EMSCRIPTEN_KEEPALIVE js_bridge_t *get_js_bridge(void) {
    return &g_bridge;
}

int shell_poll(void) {
    // Drain the bridge slot. After folding the shell into the object
    // model (proposal-shell-as-object-model-citizen.md), exactly one
    // request kind remains:
    //   1 = gs_eval(path, args)  — typed object-model call. Includes
    //                              free-form shell lines via
    //                              `shell.run`, schema queries via
    //                              `<path>.meta.*`, and tab completion
    //                              via `shell.complete` / `meta.complete`.
    // Acquire pairs with JS's Atomics.store of `pending`, which it makes after
    // writing path/args: seeing 1 here makes those bytes visible too.
    if (!__atomic_load_n(&g_bridge.pending, __ATOMIC_ACQUIRE))
        return 0;

    const char *args = (g_bridge.args[0] != '\0') ? g_bridge.args : NULL;
    gs_eval(g_bridge.path, args, g_bridge.output, JS_BRIDGE_OUTPUT_SIZE);
    // Relaxed is enough: the seq-cst store of `done` below orders it, and JS
    // writes `pending` again only after it has seen `done`.
    __atomic_store_n(&g_bridge.pending, 0, __ATOMIC_RELAXED);
    // Atomic store + wake any JS thread parked in Atomics.waitAsync on
    // `done`. Sequentially consistent so the result/output writes above
    // are visible before JS observes done == 1.
    __atomic_store_n(&g_bridge.done, 1, __ATOMIC_SEQ_CST);
    emscripten_atomic_notify((void *)&g_bridge.done, 1);
    return 1;
}

// Tab-complete and shell-line dispatch used to live here behind separate
// `pending` kinds. Both have been folded into gs_eval after the
// proposal-shell-as-object-model-citizen refactor: tab completion goes
// through `meta.complete`, free-form lines through `shell.run`. Nothing
// in this file needs to know about them anymore.

// Main tick function called by the Emscripten main loop
void em_main_tick(void) {
    tick_counter++;

    // Calculate performance metrics every PERF_UPDATE_INTERVAL ticks and
    // push them to the UI (perf proposal P12: MIPS from instr_count deltas —
    // without this, nothing in web2 would reveal a throughput regression).
    if (tick_counter % PERF_UPDATE_INTERVAL == 0) {
        double current_time = emscripten_get_now();
        uint64_t instr_now = cpu_instr_count();
        static uint64_t last_instr = 0;

        if (last_time > 0) {
            double elapsed_ms = current_time - last_time;
            ticks_per_second = (PERF_UPDATE_INTERVAL * 1000.0) / elapsed_ms;
            double mips = (double)(instr_now - last_instr) / (elapsed_ms * 1000.0);
            // clang-format off
            MAIN_THREAD_ASYNC_EM_ASM(
                { if (typeof Module.onPerfUpdate === 'function') Module.onPerfUpdate($0, $1); },
                (int)(mips * 100.0), (int)(ticks_per_second * 10.0));
            // clang-format on
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
                save_quick_checkpoint("tick-auto", false, true);
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
    // nothing (D8, F-28).
    // Re-fetch the scheduler: the request may have booted or restarted the
    // machine, freeing the one fetched above.
    if (shell_poll()) {
        scheduler_t *after = system_scheduler();
        if (!(after && scheduler_is_running(after)))
            em_video_update();
    }

    // Push a run-state notification to JS on every transition
    // (including the first tick). The callback is installed via
    // Module.onRunStateChange at module construction; ASYNC variant so
    // the worker doesn't block during emulation.
    int running = (sched && scheduler_is_running(sched)) ? 1 : 0;
    static int last_reported_running = -1;
    if (running != last_reported_running) {
        last_reported_running = running;
        // clang-format off
        MAIN_THREAD_ASYNC_EM_ASM(
            { if (typeof Module.onRunStateChange === 'function') Module.onRunStateChange(!!$0); },
            running);
        // clang-format on
    }

    // Push floppy drive present-state transitions to JS (Module.onFloppyChange),
    // same diff-and-async-invoke pattern as the run-state push above. This lets
    // the Images view clear an "Inserted" badge the instant the guest ejects a
    // disk on its own (e.g. the MacWorks loader eject) — no JS-side polling. Two
    // drives is the platform's fixed floppy maximum (see system.c do_insert_fd).
    static int last_fd_present[2] = {-1, -1};
    for (int d = 0; d < 2; d++) {
        int present = system_fd_present(d) ? 1 : 0;
        if (present != last_fd_present[d]) {
            last_fd_present[d] = present;
            // clang-format off
            MAIN_THREAD_ASYNC_EM_ASM(
                { if (typeof Module.onFloppyChange === 'function') Module.onFloppyChange($0, !!$1); },
                d, present);
            // clang-format on
        }
    }

    // Push the accelerated-mode effective CPU speed (x256; 256 = 1x) to JS on
    // change, same diff-and-async pattern as the run-state push. The value is
    // 1x outside accelerated mode and the governor steps it only on a ≥2 s
    // dwell, so this fires rarely — the status bar reads it edge-driven rather
    // than polling. JS divides by 256 for the multiplier.
    int speed_x256 = sched ? (int)scheduler_effective_speed_x256(sched) : 256;
    static int last_reported_speed = -1;
    if (speed_x256 != last_reported_speed) {
        last_reported_speed = speed_x256;
        // clang-format off
        MAIN_THREAD_ASYNC_EM_ASM(
            { if (typeof Module.onSchedulerSpeed === 'function') Module.onSchedulerSpeed($0); },
            speed_x256);
        // clang-format on
    }
}

// Exposed tick wrapper for Emscripten main loop
void tick(void) {
    em_main_tick();
}

// Forward formatted log lines to the JS-side Module.onLogEmit callback.
// Installed once at boot via log_set_sink so the new-UI Logs view can
// fan emissions out to a per-category mirror without inferring them
// from Module.print (which captures everything, not just LOG sites).
// Same MAIN_THREAD_ASYNC_EM_ASM pattern as the onRunStateChange push
// above so the worker thread never blocks on the main-thread invoke.
static void js_log_sink(const char *line, void *user) {
    (void)user;
    if (!line)
        return;
    // clang-format off
    MAIN_THREAD_ASYNC_EM_ASM(
        { if (typeof Module.onLogEmit === 'function') Module.onLogEmit(UTF8ToString($0)); },
        line);
    // clang-format on
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

// Find a mountable media file in a directory.
// Scans the directory for files that pass floppy image validation (fd probe).
// Prints the path of the first match and returns 0, or returns 1 if none found.
// Used by JS after peeler extraction (FS.readdir from main thread is broken
// with WasmFS pthreads, so this runs on the worker).
// Platform impl of gs_find_media (weak default in system.c stubs out
// for headless).  Walks `dir_path`, picks the first regular file
// recognised as a floppy image, optionally copies it to `dest`, and
// prints the path on success.  Returns 0 on success, non-zero on
// "no media found" / IO error.
int gs_find_media(const char *dir_path, const char *dest) {
    DIR *dir = opendir(dir_path);
    if (!dir) {
        printf("find-media: cannot open '%s': %s\n", dir_path, strerror(errno));
        return 1;
    }

    struct dirent *entry;
    char found_path[1024] = {0};
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.')
            continue;
        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", dir_path, entry->d_name);
        struct stat st;
        if (stat(full, &st) != 0 || !S_ISREG(st.st_mode))
            continue;
        // Try as floppy image
        image_t *img = image_open_readonly(full);
        if (img) {
            bool is_floppy = image_is_floppy(img->type);
            image_close(img);
            if (is_floppy) {
                snprintf(found_path, sizeof(found_path), "%s", full);
                break;
            }
        }
    }
    closedir(dir);

    if (!found_path[0])
        return 1;

    // Optionally copy to dest
    if (dest) {
        FILE *fin = fopen(found_path, "rb");
        if (!fin)
            return 1;
        FILE *fout = fopen(dest, "wb");
        if (!fout) {
            fclose(fin);
            return 1;
        }
        char buf[65536];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), fin)) > 0) {
            if (fwrite(buf, 1, n, fout) != n) {
                fclose(fin);
                fclose(fout);
                return 1;
            }
        }
        fclose(fin);
        fclose(fout);
    }

    printf("%s\n", found_path);
    return 0;
}

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
        // user's saved file.  Fail loudly instead. (F-1891)
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
// Per-machine layout (proposal-checkpoint-storage-isolation.md):
//   /opfs/checkpoints/<machine_id>-<created>/state.checkpoint      (current)
//   /opfs/checkpoints/<machine_id>-<created>/state.checkpoint.tmp  (in-flight)
// One file per machine; tmp+rename is the atomic swap.

#define BACKGROUND_CHECKPOINT_PATH_MAX        512
#define BACKGROUND_CHECKPOINT_MIN_INTERVAL_MS 750.0

static double g_last_background_checkpoint_ms = 0.0;
static bool g_background_handlers_installed = false;

// Build "<machine_dir>/state.checkpoint" into out_path.  Returns GS_SUCCESS
// when the machine dir is set and the path fits.
static int build_state_checkpoint_path(char *out_path, size_t out_len) {
    const char *dir = checkpoint_machine_dir();
    if (!dir)
        return GS_ERROR;
    int written = snprintf(out_path, out_len, "%s/state.checkpoint", dir);
    return (written > 0 && (size_t)written < out_len) ? GS_SUCCESS : GS_ERROR;
}

// Find the path to the current valid background checkpoint.
// Overrides the weak default in system.c for the WASM platform.
// Returns a static buffer with the path, or NULL if none found.
const char *find_valid_checkpoint_path(void) {
    static char path_buf[BACKGROUND_CHECKPOINT_PATH_MAX];
    if (build_state_checkpoint_path(path_buf, sizeof(path_buf)) != GS_SUCCESS)
        return NULL;
    struct stat st;
    if (stat(path_buf, &st) != 0)
        return NULL;
    // Reject checkpoints from a different build (incompatible state layout)
    if (!checkpoint_validate_build_id(path_buf))
        return NULL;
    return path_buf;
}

// Save a quick checkpoint via tmp+rename inside the per-machine directory.
static int save_quick_checkpoint(const char *reason, bool verbose, bool rate_limit) {
    scheduler_t *sched = system_scheduler();
    if (!sched)
        return GS_ERROR;

    // Skip checkpointing when the emulator is idle — nothing meaningful to save
    if (!scheduler_is_running(sched) && cpu_instr_count() == 0)
        return GS_SUCCESS;

    // No machine identity yet → nothing to save under.
    if (!checkpoint_machine_dir()) {
        if (verbose)
            printf("[checkpoint] no machine directory set, skipping quick checkpoint\n");
        return GS_SUCCESS;
    }

    double now = emscripten_get_now();

    if (rate_limit && g_last_background_checkpoint_ms > 0.0) {
        double delta = now - g_last_background_checkpoint_ms;
        if (delta >= 0.0 && delta < BACKGROUND_CHECKPOINT_MIN_INTERVAL_MS)
            return GS_SUCCESS;
    }

    char final_path[BACKGROUND_CHECKPOINT_PATH_MAX];
    char tmp_path[BACKGROUND_CHECKPOINT_PATH_MAX];
    if (build_state_checkpoint_path(final_path, sizeof(final_path)) != GS_SUCCESS)
        return GS_ERROR;
    int wn = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", final_path);
    if (wn <= 0 || (size_t)wn >= sizeof(tmp_path))
        return GS_ERROR;

    // Record running state before stopping - this will be saved in the checkpoint
    bool was_running = scheduler_is_running(sched);
    if (was_running)
        scheduler_stop(sched);

    // Temporarily restore running flag so checkpoint captures the pre-stop state
    if (was_running && sched)
        scheduler_set_running(sched, true);

    double checkpoint_start_time = emscripten_get_now();

    // Drop any stale tmp from a crashed prior run.
    unlink(tmp_path);
    int rc = system_checkpoint(tmp_path, CHECKPOINT_KIND_QUICK);
    if (rc == GS_SUCCESS) {
        if (rename(tmp_path, final_path) != 0) {
            printf("[checkpoint] rename %s -> %s failed: %s\n", tmp_path, final_path, strerror(errno));
            unlink(tmp_path);
            rc = GS_ERROR;
        }
    } else {
        unlink(tmp_path);
    }

    double checkpoint_elapsed_ms = emscripten_get_now() - checkpoint_start_time;

    if (rc == GS_SUCCESS) {
        g_last_background_checkpoint_ms = now;
        // Status-bar heartbeat: push the save duration so the CP glyph
        // flashes and its tooltip updates. x100 fixed-point since
        // MAIN_THREAD_ASYNC_EM_ASM carries ints.
        // clang-format off
        MAIN_THREAD_ASYNC_EM_ASM(
            { if (typeof Module.onCheckpointSaved === 'function') Module.onCheckpointSaved($0); },
            (int)(checkpoint_elapsed_ms * 100.0));
        // clang-format on
        if (verbose)
            printf("Checkpoint saved to %s (%.2f ms)\n", final_path, checkpoint_elapsed_ms);
    } else if (verbose) {
        printf("[checkpoint] quick checkpoint failed (%s)\n", reason ? reason : "background");
    }
    return rc;
}

// Request background checkpoint (with rate limiting)
static void maybe_request_background_checkpoint(const char *reason, bool rate_limit) {
    int rc = save_quick_checkpoint(reason, false, rate_limit);
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

// Background checkpoint command
// Platform impl of gs_background_checkpoint (weak default in system.c
// stubs out for headless).
int gs_background_checkpoint(const char *reason) {
    int rc = save_quick_checkpoint(reason ? reason : "manual", true, false);
    return (rc == GS_SUCCESS) ? 0 : -1;
}

// Forward declaration — definition is below.
static int clear_checkpoint_files(void);

// Platform impl of gs_checkpoint_clear / gs_register_machine.  Both
// only mean something on WASM (where OPFS hosts per-machine
// checkpoint directories); headless gets the weak no-op stubs.
int gs_checkpoint_clear(void) {
    int removed = clear_checkpoint_files();
    printf("Cleared %d checkpoint file(s)\n", removed);
    return 0;
}

int gs_register_machine(const char *machine_id, const char *created) {
    if (!machine_id || !created)
        return -1;
    int rc = checkpoint_machine_set(machine_id, created);
    if (rc != 0)
        printf("register_machine: failed to set %s-%s\n", machine_id, created);
    return rc == 0 ? 0 : -1;
}

// Clear checkpoint files inside the current machine directory.  Drops
// state.checkpoint, any leftover *.tmp, and (defensive) any legacy
// sequence-numbered *.checkpoint / *.pending / *.complete files.  The
// machine directory itself is left in place.
static int clear_checkpoint_files(void) {
    const char *dir_path = checkpoint_machine_dir();
    if (!dir_path)
        return 0;
    DIR *dir = opendir(dir_path);
    if (!dir)
        return 0;
    struct dirent *entry;
    int removed = 0;
    char path[BACKGROUND_CHECKPOINT_PATH_MAX];
    while ((entry = readdir(dir)) != NULL) {
        const char *name = entry->d_name;
        if (!name || name[0] == '.')
            continue;
        size_t len = strlen(name);
        bool match = false;
        if (strcmp(name, "state.checkpoint") == 0)
            match = true;
        else if (len >= 4 && strcmp(name + len - 4, ".tmp") == 0)
            match = true;
        else if (len >= 11 && strcmp(name + len - 11, ".checkpoint") == 0)
            match = true; // legacy
        else if (strstr(name, ".complete") || strstr(name, ".pending"))
            match = true; // legacy
        if (match) {
            snprintf(path, sizeof(path), "%s/%s", dir_path, name);
            if (unlink(path) == 0)
                removed++;
        }
    }
    closedir(dir);
    return removed;
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
    // during global constructors on the main thread; see wasmfs-opfs-root-limitation.md).
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
    // content).  The platform owns the filesystem; core never enumerates a
    // directory or builds a path.  Mid-session uploads are offered by the
    // web app's ingest path (machine.vrom.offer), so this startup pass only
    // needs to cover what already persisted.
    DIR *vrom_dir = opendir("/opfs/images/vrom");
    if (vrom_dir) {
        struct dirent *entry;
        char vrom_path[512];
        while ((entry = readdir(vrom_dir)) != NULL) {
            if (entry->d_name[0] == '.')
                continue;
            if (snprintf(vrom_path, sizeof(vrom_path), "/opfs/images/vrom/%s", entry->d_name) >= (int)sizeof(vrom_path))
                continue;
            vrom_offer(vrom_path);
        }
        closedir(vrom_dir);
    }

    // ...and the same pass over the persistent PCI expansion-ROM store, for
    // the same reason: without it a .prom that persisted in an earlier
    // session is invisible after a reload, and the card it drives looks
    // uninstallable until the user uploads the file again.
    DIR *prom_dir = opendir("/opfs/images/prom");
    if (prom_dir) {
        struct dirent *entry;
        char prom_path[512];
        while ((entry = readdir(prom_dir)) != NULL) {
            if (entry->d_name[0] == '.')
                continue;
            if (snprintf(prom_path, sizeof(prom_path), "/opfs/images/prom/%s", entry->d_name) >= (int)sizeof(prom_path))
                continue;
            prom_offer(prom_path);
        }
        closedir(prom_dir);
    }

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

    // Route every log_emit through Module.onLogEmit so the new-UI Logs
    // view gets a structured stream parallel to stdout. shell_init has
    // already called log_init; setting the sink here also forwards any
    // categories registered later (setup_init, machine boot, …).
    log_set_sink(js_log_sink, NULL);

    // Bridge is open for business. JS gates its first gsEval on this
    // flag so requests issued during the boot window don't dispatch
    // against the empty default root class. The notify wakes any JS
    // thread parked in Atomics.waitAsync on this field.
    __atomic_store_n(&g_bridge.ready, 1, __ATOMIC_SEQ_CST);
    emscripten_atomic_notify((void *)&g_bridge.ready, INT_MAX);

    // Initialize subsystems (safe without a machine — video and audio handle NULL)
    em_video_init();
    em_audio_init();
    em_camera_init(); // announce the webcam frame transport to the main thread
    em_audio_in_init(); // ...and the microphone sample ring
    setup_pointer_lock();

    install_background_checkpoint_handlers();

    // Assertion callback is installed automatically by system_post_create()
    // whenever a machine is created.

    emscripten_set_main_loop(tick, 0, 1); // Use RAF, simulate infinite loop
    return 0;
}

// ============================================================================
// Assertion Notification for JavaScript
// ============================================================================

// Platform-specific assertion callback implementation.
// Notifies the browser (Playwright tests) that an assertion has failed.
// Must run on main thread (accesses window.__gsAssertionHandler).
static void em_assertion_callback(const char *kind, const char *expr, const char *file, int line, const char *func) {
    if (!expr)
        expr = kind;
    // clang-format off
    MAIN_THREAD_EM_ASM(
        {
            var exprStr = $0 ? UTF8ToString($0) : "";
            var fileStr = $1 ? UTF8ToString($1) : "<unknown>";
            var lineNum = $2;
            var funcStr = $3 ? UTF8ToString($3) : "<unknown>";

            // Call global handler if registered
            if (typeof window !== 'undefined' && typeof window.__gsAssertionHandler === 'function') {
                try {
                    window.__gsAssertionHandler(
                        {expr: exprStr, file: fileStr, line: lineNum, func: funcStr, timestamp: Date.now()});
                } catch (e) {
                    // Handler may throw to stop execution - this is expected
                }
            }
        },
        expr, file, line, func);
    // clang-format on
}

// Background auto-checkpoint accessors — override the weak defaults in
// system.c so the `auto_checkpoint` attribute reads/writes the live flag.
bool gs_checkpoint_auto_get(void) {
    return checkpoint_auto_enabled;
}

void gs_checkpoint_auto_set(bool enabled) {
    checkpoint_auto_enabled = enabled;
    if (!enabled)
        checkpoint_tick_counter = 0;
}

// The always-present AppleShare volume.  The path literal lives here, in the
// platform layer, because core never fabricates or interprets a path (PR #69);
// `appletalk_server.c` only ever executes the tree operation it is handed.
// Under OPFS the directory — and the AppleDouble sidecars the AFP server
// writes beside each file — persist across page reloads for free.
#define GS_DEFAULT_SHARE_NAME "Shared"
#define GS_DEFAULT_SHARE_PATH "/opfs/shared"

// Publish the default share.  Idempotent: a machine teardown drops the volume
// table, so this runs again on every system_create.  Failure is a logged
// warning, never a boot error — a user who removed the directory should still
// get a running machine.
static void provision_default_share(void) {
    if (atalk_afp_volume_find(GS_DEFAULT_SHARE_NAME) >= 0)
        return;
    if (mkdir(GS_DEFAULT_SHARE_PATH, 0777) != 0 && errno != EEXIST) {
        printf("[C] default share: cannot create %s (%s)\n", GS_DEFAULT_SHARE_PATH, strerror(errno));
        return;
    }
    char err[192];
    if (atalk_afp_volume_add(GS_DEFAULT_SHARE_NAME, GS_DEFAULT_SHARE_PATH, err, sizeof(err)) < 0)
        printf("[C] default share: %s\n", err);
}

// Platform hook: publish the default share after each system_create.
void system_post_create(config_t *cfg) {
    (void)cfg;
    provision_default_share();
}

// ============================================================================
// Diagnostics - Host Callstack
// ============================================================================

// Print the host (Emscripten/WASM or native) callstack for debugging
void em_print_host_callstack(void) {
    printf("\n=== Host callstack ===\n");
#ifdef __EMSCRIPTEN__
    // Print both C and JS stacks
    char stackbuf[8192];
    int n =
        emscripten_get_callstack(EM_LOG_C_STACK | EM_LOG_JS_STACK | EM_LOG_NO_PATHS, stackbuf, (int)sizeof(stackbuf));
    if (n > 0)
        printf("%s\n", stackbuf);
    else
        printf("(unavailable)\n");
#else
// Attempt to use glibc backtrace on native builds
#if defined(__linux__) || defined(__APPLE__)
    void *buffer[64];
    int n = backtrace(buffer, 64);
    char **syms = backtrace_symbols(buffer, n);
    if (syms) {
        for (int i = 0; i < n; i++)
            printf("%s\n", syms[i]);
        free(syms);
    } else {
        printf("(unavailable)\n");
    }
#else
    printf("(unavailable)\n");
#endif
#endif
}

// Platform-specific callstack function (exposed to core via platform.h)
void platform_print_host_callstack(void) {
    em_print_host_callstack();
}
