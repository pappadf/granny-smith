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
#include <emscripten/version.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
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
#include "checkpoint.h"
#include "checkpoint_machine.h"
#include "cmd_types.h"
#include "cpu.h"
#include "keyboard.h"
#include "machine.h"
#include "mouse.h"
#include "platform.h"
#include "scheduler.h"
#include "shell.h"
#include "system.h"

// ============================================================================
// Forward Declarations
// ============================================================================

static void em_assertion_callback(const char *expr, const char *file, int line, const char *func);

// Deferred speed mode: saved at parse time, applied in system_post_create()
// when the machine (and scheduler) are created later via rom load.
static enum schedule_mode g_deferred_speed = schedule_real_time;
static bool g_deferred_speed_set = false;

// ============================================================================
// Pointer-lock and Input Handling
// ============================================================================

static volatile int pointer_locked = 0;
static bool mouse_button_down = false;

// Forward declarations for input callbacks
static void setup_pointer_lock(void);
static EM_BOOL mouse_down_cb(int, const EmscriptenMouseEvent *, void *);
static EM_BOOL mouse_up_cb(int, const EmscriptenMouseEvent *, void *);
static EM_BOOL plock_change_cb(int, const EmscriptenPointerlockChangeEvent *, void *);
static EM_BOOL mouse_move_cb(int, const EmscriptenMouseEvent *, void *);
static EM_BOOL key_down_cb(int, const EmscriptenKeyboardEvent *, void *);
static EM_BOOL key_up_cb(int, const EmscriptenKeyboardEvent *, void *);

// Mouse movement handler
static void emulator_mouse_move(bool button, int dx, int dy) {
    if (!pointer_locked)
        return; // Ignore mouse movement if pointer is not locked

    // Call the actual mouse update routine
    system_mouse_update(button, dx, dy);
}

// Mouse button down callback
static EM_BOOL mouse_down_cb(int type, const EmscriptenMouseEvent *e, void *ud) {
    (void)type;
    (void)ud;
    if (!pointer_locked)
        emscripten_request_pointerlock("#screen", EM_FALSE);
    mouse_button_down = true;
    emulator_mouse_move(mouse_button_down, e->movementX, e->movementY);
    return EM_TRUE;
}

// Mouse button up callback
static EM_BOOL mouse_up_cb(int type, const EmscriptenMouseEvent *e, void *ud) {
    (void)type;
    (void)ud;
    mouse_button_down = false;
    emulator_mouse_move(mouse_button_down, e->movementX, e->movementY);
    return EM_TRUE;
}

// Pointer lock change callback
static EM_BOOL plock_change_cb(int type, const EmscriptenPointerlockChangeEvent *e, void *ud) {
    (void)type;
    (void)ud;
    pointer_locked = e->isActive;
    return EM_TRUE;
}

// Mouse move callback
static EM_BOOL mouse_move_cb(int type, const EmscriptenMouseEvent *e, void *ud) {
    (void)type;
    (void)ud;
    if (!pointer_locked)
        return EM_FALSE;
    emulator_mouse_move(mouse_button_down, e->movementX, e->movementY);
    return EM_TRUE;
}

// ============================================================================
// Keyboard Mapping (DOM to Macintosh ADB)
// ============================================================================

// Map DOM keyboard codes to Macintosh ADB virtual key codes.
// ADB virtual codes are the standard key identifiers from Inside Macintosh Vol V.
// The keyboard.c module translates these to Mac Plus raw codes for the VIA protocol.
static int map_dom_code_to_mac(const char *code, const char *key) {
    (void)key; // key parameter kept for potential future use
    if (!code)
        return -1;

    // ── Letters (A-Z) ───────────────────────────────────────────────────────
    if (!strcmp(code, "KeyA"))
        return 0x00;
    if (!strcmp(code, "KeyS"))
        return 0x01;
    if (!strcmp(code, "KeyD"))
        return 0x02;
    if (!strcmp(code, "KeyF"))
        return 0x03;
    if (!strcmp(code, "KeyH"))
        return 0x04;
    if (!strcmp(code, "KeyG"))
        return 0x05;
    if (!strcmp(code, "KeyZ"))
        return 0x06;
    if (!strcmp(code, "KeyX"))
        return 0x07;
    if (!strcmp(code, "KeyC"))
        return 0x08;
    if (!strcmp(code, "KeyV"))
        return 0x09;
    if (!strcmp(code, "KeyB"))
        return 0x0B;
    if (!strcmp(code, "KeyQ"))
        return 0x0C;
    if (!strcmp(code, "KeyW"))
        return 0x0D;
    if (!strcmp(code, "KeyE"))
        return 0x0E;
    if (!strcmp(code, "KeyR"))
        return 0x0F;
    if (!strcmp(code, "KeyY"))
        return 0x10;
    if (!strcmp(code, "KeyT"))
        return 0x11;
    if (!strcmp(code, "KeyU"))
        return 0x20;
    if (!strcmp(code, "KeyI"))
        return 0x22;
    if (!strcmp(code, "KeyO"))
        return 0x1F;
    if (!strcmp(code, "KeyP"))
        return 0x23;
    if (!strcmp(code, "KeyL"))
        return 0x25;
    if (!strcmp(code, "KeyJ"))
        return 0x26;
    if (!strcmp(code, "KeyK"))
        return 0x28;
    if (!strcmp(code, "KeyN"))
        return 0x2D;
    if (!strcmp(code, "KeyM"))
        return 0x2E;

    // ── Number row (0-9 and symbols) ────────────────────────────────────────
    if (!strcmp(code, "Digit1"))
        return 0x12;
    if (!strcmp(code, "Digit2"))
        return 0x13;
    if (!strcmp(code, "Digit3"))
        return 0x14;
    if (!strcmp(code, "Digit4"))
        return 0x15;
    if (!strcmp(code, "Digit5"))
        return 0x17;
    if (!strcmp(code, "Digit6"))
        return 0x16;
    if (!strcmp(code, "Digit7"))
        return 0x1A;
    if (!strcmp(code, "Digit8"))
        return 0x1C;
    if (!strcmp(code, "Digit9"))
        return 0x19;
    if (!strcmp(code, "Digit0"))
        return 0x1D;
    if (!strcmp(code, "Minus"))
        return 0x1B; // - / _
    if (!strcmp(code, "Equal"))
        return 0x18; // = / +

    // ── Punctuation and brackets ────────────────────────────────────────────
    if (!strcmp(code, "BracketLeft"))
        return 0x21; // [ / {
    if (!strcmp(code, "BracketRight"))
        return 0x1E; // ] / }
    if (!strcmp(code, "Backslash"))
        return 0x2A; // \ / |
    if (!strcmp(code, "Semicolon"))
        return 0x29; // ; / :
    if (!strcmp(code, "Quote"))
        return 0x27; // ' / "
    if (!strcmp(code, "Comma"))
        return 0x2B; // , / <
    if (!strcmp(code, "Period"))
        return 0x2F; // . / >
    if (!strcmp(code, "Slash"))
        return 0x2C; // / / ?
    if (!strcmp(code, "Backquote"))
        return 0x32; // ` / ~
    if (!strcmp(code, "IntlBackslash"))
        return 0x0A; // § / ± (non-US ISO layout)

    // ── Control keys ────────────────────────────────────────────────────────
    if (!strcmp(code, "Tab"))
        return 0x30;
    if (!strcmp(code, "Space"))
        return 0x31;
    if (!strcmp(code, "Backspace"))
        return 0x33; // Delete key on Mac
    if (!strcmp(code, "Enter"))
        return 0x24; // Return
    if (!strcmp(code, "Escape"))
        return 0x35;

    // ── Modifier keys ───────────────────────────────────────────────────────
    // Mac Plus has single Shift/Option/Command keys but we map both left/right
    if (!strcmp(code, "ControlLeft"))
        return 0x36;
    if (!strcmp(code, "ControlRight"))
        return 0x36;
    if (!strcmp(code, "ShiftLeft"))
        return 0x38;
    if (!strcmp(code, "ShiftRight"))
        return 0x38;
    if (!strcmp(code, "CapsLock"))
        return 0x39;
    if (!strcmp(code, "AltLeft"))
        return 0x3A; // Option (left)
    if (!strcmp(code, "AltRight"))
        return 0x3A; // Option (right)
    if (!strcmp(code, "MetaLeft"))
        return 0x37; // Command (left)
    if (!strcmp(code, "MetaRight"))
        return 0x37; // Command (right)
    if (!strcmp(code, "OSLeft"))
        return 0x37; // Command (Windows key)
    if (!strcmp(code, "OSRight"))
        return 0x37; // Command (Windows key)

    // ── Arrow keys ──────────────────────────────────────────────────────────
    // ADB virtual codes 0x7B-0x7E; keyboard.c translates to Mac Plus raw codes
    if (!strcmp(code, "ArrowLeft"))
        return 0x7B;
    if (!strcmp(code, "ArrowRight"))
        return 0x7C;
    if (!strcmp(code, "ArrowDown"))
        return 0x7D;
    if (!strcmp(code, "ArrowUp"))
        return 0x7E;

    // ── Numeric keypad ──────────────────────────────────────────────────────
    if (!strcmp(code, "NumpadDecimal"))
        return 0x41; // Keypad .
    if (!strcmp(code, "NumpadMultiply"))
        return 0x43; // Keypad *
    if (!strcmp(code, "NumpadAdd"))
        return 0x45; // Keypad +
    if (!strcmp(code, "NumLock"))
        return 0x47; // Keypad Clear
    if (!strcmp(code, "NumpadDivide"))
        return 0x4B; // Keypad /
    if (!strcmp(code, "NumpadEnter"))
        return 0x4C; // Keypad Enter
    if (!strcmp(code, "NumpadSubtract"))
        return 0x4E; // Keypad -
    if (!strcmp(code, "NumpadEqual"))
        return 0x51; // Keypad =
    if (!strcmp(code, "Numpad0"))
        return 0x52;
    if (!strcmp(code, "Numpad1"))
        return 0x53;
    if (!strcmp(code, "Numpad2"))
        return 0x54;
    if (!strcmp(code, "Numpad3"))
        return 0x55;
    if (!strcmp(code, "Numpad4"))
        return 0x56;
    if (!strcmp(code, "Numpad5"))
        return 0x57;
    if (!strcmp(code, "Numpad6"))
        return 0x58;
    if (!strcmp(code, "Numpad7"))
        return 0x59;
    if (!strcmp(code, "Numpad8"))
        return 0x5B;
    if (!strcmp(code, "Numpad9"))
        return 0x5C;

    return -1; // unmapped key
}

// Key down callback
static EM_BOOL key_down_cb(int type, const EmscriptenKeyboardEvent *e, void *ud) {
    (void)type;
    (void)ud;
    if (!pointer_locked)
        return EM_FALSE;
    int k = map_dom_code_to_mac(e->code, e->key);
    if (k < 0)
        return EM_FALSE;
    system_keyboard_update(key_down, k);
    return EM_TRUE;
}

// Key up callback
static EM_BOOL key_up_cb(int type, const EmscriptenKeyboardEvent *e, void *ud) {
    (void)type;
    (void)ud;
    if (!pointer_locked)
        return EM_FALSE;
    int k = map_dom_code_to_mac(e->code, e->key);
    if (k < 0)
        return EM_FALSE;
    system_keyboard_update(key_up, k);
    return EM_TRUE;
}

// Setup pointer lock callbacks
static void setup_pointer_lock(void) {
    emscripten_set_mousedown_callback("#screen", NULL, EM_TRUE, mouse_down_cb);
    emscripten_set_mouseup_callback("#screen", NULL, EM_TRUE, mouse_up_cb);
    emscripten_set_pointerlockchange_callback(EMSCRIPTEN_EVENT_TARGET_DOCUMENT, NULL, EM_TRUE, plock_change_cb);
    emscripten_set_mousemove_callback(EMSCRIPTEN_EVENT_TARGET_DOCUMENT, NULL, EM_TRUE, mouse_move_cb);
    emscripten_set_keydown_callback(EMSCRIPTEN_EVENT_TARGET_DOCUMENT, NULL, EM_TRUE, key_down_cb);
    emscripten_set_keyup_callback(EMSCRIPTEN_EVENT_TARGET_DOCUMENT, NULL, EM_TRUE, key_up_cb);
}

// ============================================================================
// Main Loop Timing and Execution
// ============================================================================

#define PERF_UPDATE_INTERVAL 60 // Calculate performance every X ticks
#define SHELL_POLL_INTERVAL  6 // Poll shell every Y ticks when idle
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
// forbidden.
//
// The single shared-memory region. Layout in em.h, mirrored in
// emulator.js. Buffer sizes (path, args, output) are tuned for current
// peak usage: longest paths are checkpoint paths under /opfs, args
// carry JSON arrays of primitive values, output carries `meta.*`
// introspection dumps which dominate.
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
    if (!g_bridge.pending)
        return 0;

    const char *args = (g_bridge.args[0] != '\0') ? g_bridge.args : NULL;
    int rc = gs_eval(g_bridge.path, args, g_bridge.output, JS_BRIDGE_OUTPUT_SIZE);
    g_bridge.result = rc;
    g_bridge.pending = 0;
    // Atomic store + wake any JS thread parked in Atomics.waitAsync on
    // `done`. Sequentially consistent so the result/output writes above
    // are visible before JS observes done == 1.
    __atomic_store_n(&g_bridge.done, 1, __ATOMIC_SEQ_CST);
    emscripten_atomic_notify((void *)&g_bridge.done, 1);
    return 1;
}

// Startup command runner
static void run_startup_command(const char *line) {
    if (!line || !*line)
        return;

    size_t len = strlen(line);
    char *buffer = (char *)malloc(len + 1);
    if (!buffer) {
        fprintf(stderr, "startup command alloc failed for '%s'\n", line);
        return;
    }

    memcpy(buffer, line, len + 1);
    shell_dispatch(buffer);
    free(buffer);
}

// Main tick function called by the Emscripten main loop
void em_main_tick(void) {
    tick_counter++;

    // Calculate performance metrics every PERF_UPDATE_INTERVAL ticks
    if (tick_counter % PERF_UPDATE_INTERVAL == 0) {
        double current_time = emscripten_get_now();

        if (last_time > 0) {
            double elapsed_ms = current_time - last_time;
            ticks_per_second = (PERF_UPDATE_INTERVAL * 1000.0) / elapsed_ms;
        }

        last_time = current_time;
    }

    // Execute based on emulation state
    scheduler_t *sched = system_scheduler();
    if (sched && scheduler_is_running(sched)) {
        static uint64_t last_total_instructions = 0;
        static double last_print_time = 0.0;

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

        // Print rolling average of instructions per second once every second
        if (last_print_time == 0.0) {
            last_print_time = now;
            last_total_instructions = cpu_instr_count();
        } else if (now - last_print_time >= 1000.0) {
            last_print_time = now;
            last_total_instructions = cpu_instr_count();
        }
    } else {
        assert(!sched || !scheduler_is_running(sched));
    }

    // Poll for pending shell commands every tick.  This must run regardless
    // of running state so that drag-and-drop media inserts, checkpoint
    // commands, etc. execute while the emulator is running.
    shell_poll();

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
}

// Exposed tick wrapper for Emscripten main loop
void tick(void) {
    em_main_tick();
}

// Print usage
static void print_usage(const char *program_name) {
    printf("Usage: %s [options]\n", program_name);
    printf("Options:\n");
    printf("  --model=MODEL    Specify the model type (e.g., plus)\n");
    printf("  --speed=MODE     Scheduler mode (max|realtime|hardware)\n");
    printf("  --help           Display this help message\n");
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
            bool is_floppy = (img->type == image_fd_ss || img->type == image_fd_ds || img->type == image_fd_hd);
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

// Download command - save file to browser
// Platform impl of gs_download (weak default in system.c stubs out).
int gs_download(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        printf("download: cannot access '%s': %s\n", path, strerror(errno));
        return 0;
    }
    if (!S_ISREG(st.st_mode)) {
        printf("download: '%s' is not a regular file\n", path);
        return 0;
    }

    // Read file on the worker thread (OPFS accessible here)
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("download: cannot open '%s': %s\n", path, strerror(errno));
        return 0;
    }
    size_t file_size = (size_t)st.st_size;
    uint8_t *buf = (uint8_t *)malloc(file_size);
    if (!buf) {
        fclose(f);
        printf("download: out of memory (%zu bytes)\n", file_size);
        return 0;
    }
    size_t nread = fread(buf, 1, file_size, f);
    fclose(f);

    // Extract filename from path
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;

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

// Beforeunload callback
static const char *background_beforeunload_callback(int eventType, const void *reserved, void *userData) {
    (void)eventType;
    (void)reserved;
    maybe_request_background_checkpoint((const char *)userData, true);
    return NULL;
}

// Install background checkpoint handlers
static void install_background_checkpoint_handlers(void) {
    if (g_background_handlers_installed)
        return;
    emscripten_set_visibilitychange_callback((void *)"visibilitychange", EM_FALSE, background_visibility_callback);
    emscripten_set_beforeunload_callback((void *)"beforeunload", background_beforeunload_callback);
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

int main(int argc, char *argv[]) {
    signal(SIGINT, sigint_handler);

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
    mkdir("/opfs/images/fd", 0777);
    mkdir("/opfs/images/fdhd", 0777);
    mkdir("/opfs/images/hd", 0777);
    mkdir("/opfs/images/cd", 0777);
    mkdir("/opfs/checkpoints", 0777);
    mkdir("/opfs/upload", 0777);

    // Volatile scratch space on memory backend (visible from all threads).
    backend_t membk = wasmfs_create_memory_backend();
    wasmfs_create_directory("/tmp", 0777, membk);
    mkdir("/tmp/upload", 0777);
    mkdir("/tmp/extract", 0777);

    // Define the supported options
    static struct option long_options[] = {
        {"model", required_argument, 0, 'm'},
        {"help",  no_argument,       0, 'h'},
        {"speed", required_argument, 0, 's'},
        {0,       0,                 0, 0  }
    };

    // Variables to store parsed options
    int option_index = 0;
    int c;
    char *model = NULL;
    char *speed_mode = NULL;

    // Parse command-line options
    while ((c = getopt_long(argc, argv, "m:hs:", long_options, &option_index)) != -1) {
        switch (c) {
        case 'm':
            model = optarg;
            break;
        case 'h':
            print_usage(argv[0]);
            return 0;
        case 's':
            speed_mode = optarg;
            break;
        case '?':
            print_usage(argv[0]);
            return 1;
        default:
            break;
        }
    }

    shell_init();
    setup_init();

    // Bridge is open for business. JS gates its first gsEval on this
    // flag so requests issued during the boot window don't dispatch
    // against the empty default root class. The notify wakes any JS
    // thread parked in Atomics.waitAsync on this field.
    __atomic_store_n(&g_bridge.ready, 1, __ATOMIC_SEQ_CST);
    emscripten_atomic_notify((void *)&g_bridge.ready, INT_MAX);

    // Deferred machine instantiation: global_emulator stays NULL until a ROM
    // is loaded via the `rom load` command, which identifies the machine type
    // and creates it automatically.  If --model was provided, create it now
    // for backward compatibility.
    if (model) {
        const hw_profile_t *profile = machine_find(model);
        if (profile) {
            global_emulator = system_create(profile, NULL);
            printf("%s (%u KB RAM)\n", profile->name, profile->ram_default / 1024);
        } else {
            printf("Unknown model: %s\n", model);
        }
    }

    // Parse and save speed mode for deferred application.
    // When the machine already exists (--model was given), apply immediately.
    // Otherwise, system_post_create() will apply it when rom load creates the machine.
    if (speed_mode) {
        if (strcmp(speed_mode, "max") == 0) {
            g_deferred_speed = schedule_max_speed;
            g_deferred_speed_set = true;
        } else if (strcmp(speed_mode, "realtime") == 0 || strcmp(speed_mode, "real") == 0) {
            g_deferred_speed = schedule_real_time;
            g_deferred_speed_set = true;
        } else if (strcmp(speed_mode, "hardware") == 0 || strcmp(speed_mode, "hw") == 0 ||
                   strcmp(speed_mode, "accuracy") == 0) {
            g_deferred_speed = schedule_hw_accuracy;
            g_deferred_speed_set = true;
        } else {
            printf("[C] Unknown --speed mode '%s' (valid: max|realtime|hardware)\n", speed_mode);
        }

        scheduler_t *sched = system_scheduler();
        if (sched && g_deferred_speed_set) {
            scheduler_set_mode(sched, g_deferred_speed);
            printf("[C] Scheduler mode set via --speed=%s\n", speed_mode);
        }
    }

    // Initialize subsystems (safe without a machine — video and audio handle NULL)
    em_video_init();
    em_audio_init();
    setup_pointer_lock();

    // Phase 5c — legacy WASM-platform shell command registrations
    // retired. Typed root methods (download, find_media, checkpoint_*,
    // background_checkpoint, beep) replace them; cmd_* bodies are kept
    // when the typed wrappers still call them.

    install_background_checkpoint_handlers();

    // Assertion callback is installed automatically by system_post_create()
    // whenever a machine is created (either here via --model or later via rom load).

    emscripten_set_main_loop(tick, 0, 1); // Use RAF, simulate infinite loop
    return 0;
}

// ============================================================================
// Assertion Notification for JavaScript
// ============================================================================

// Platform-specific assertion callback implementation.
// Notifies the browser (Playwright tests) that an assertion has failed.
// Must run on main thread (accesses window.__gsAssertionHandler).
static void em_assertion_callback(const char *expr, const char *file, int line, const char *func) {
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

// Platform hook: install assertion callback and apply deferred speed mode
// after each system_create (including deferred creation via rom load).
void system_post_create(config_t *cfg) {
    debug_t *debug = system_debug();
    if (debug) {
        debug->assertion_callback = em_assertion_callback;
    }

    // Apply deferred speed mode from --speed flag parsed at startup
    if (g_deferred_speed_set) {
        scheduler_t *sched = system_scheduler();
        if (sched) {
            scheduler_set_mode(sched, g_deferred_speed);
            const char *name = "unknown";
            switch (g_deferred_speed) {
            case schedule_max_speed:
                name = "max";
                break;
            case schedule_real_time:
                name = "realtime";
                break;
            case schedule_hw_accuracy:
                name = "hardware";
                break;
            }
            printf("[C] Deferred scheduler mode applied: --speed=%s\n", name);
        }
    }
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
