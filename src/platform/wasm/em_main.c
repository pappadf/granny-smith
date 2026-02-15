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
#include <emscripten/emscripten.h>
#include <emscripten/html5.h>
#include <emscripten/version.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
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
#else
#if defined(__linux__) || defined(__APPLE__)
#include <execinfo.h>
#endif
#endif

#include "checkpoint.h"
#include "cpu.h"
#include "keyboard.h"
#include "mouse.h"
#include "platform.h"
#include "scheduler.h"
#include "shell.h"
#include "system.h"

// ============================================================================
// Forward Declarations
// ============================================================================

static void em_assertion_callback(const char *expr, const char *file, int line, const char *func);

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
static int last_is_running = -1;

// Shell prompt helper
static void build_prompt_text(char *dest, size_t dest_len) {
    if (!dest || dest_len == 0)
        return;
    dest[0] = '\0';
    const char *fallback = "mini> ";
    if (!system_is_initialized()) {
        snprintf(dest, dest_len, "%s", fallback);
        return;
    }
    debugger_disasm_pc(dest);
    size_t used = strnlen(dest, dest_len - 1);
    if (used >= dest_len - 3)
        used = dest_len - 4;
    dest[used++] = ' ';
    dest[used++] = '>';
    dest[used++] = ' ';
    dest[used] = '\0';
}

// Emscripten-specific shell stubs
void print_prompt(void) {}

EMSCRIPTEN_KEEPALIVE const char *shell_emit_prompt(void) {
    static char prompt[100];
    build_prompt_text(prompt, sizeof(prompt));
    return prompt;
}

int shell_poll(void) {
    return 0;
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

// Auto-attach SCSI drives from /persist/boot
static void auto_attach_scsi_drives(struct config *cfg) {
    DIR *dir = opendir("/persist/boot");
    if (!dir) {
        return;
    }

    struct dirent *entry;
    for (int i = 1; i <= 7; ++i) {
        char expected[16];
        snprintf(expected, sizeof(expected), "hd%d.img", i);

        rewinddir(dir);
        while ((entry = readdir(dir)) != NULL) {
            if (strcmp(entry->d_name, expected) == 0) {
                char filename[256];
                snprintf(filename, sizeof(filename), "/persist/boot/%s", expected);
                add_scsi_drive(cfg, filename, i - 1);
                break;
            }
        }
    }
    closedir(dir);
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

        // Only poll shell periodically when idle to reduce CPU usage
        if (tick_counter % SHELL_POLL_INTERVAL == 0) {
            shell_poll();
        }
    }

    // Notify JS when run state changes
    {
        int cur_is_running = (sched && scheduler_is_running(sched)) ? 1 : 0;
        if (cur_is_running != last_is_running) {
            last_is_running = cur_is_running;
            // clang-format off
            EM_ASM(
                {
                    try {
                        if (Module && Module.onRunStateChange)
                            Module.onRunStateChange(!!$0);
                    } catch (e) {
                        console.warn('onRunStateChange callback error', e);
                    }
                },
                cur_is_running);
            // clang-format on
        }
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

// Signal handler
static volatile sig_atomic_t got_sigint = 0;

void sigint_handler(int sig) {
    (void)sig;
    got_sigint = 1;
    scheduler_t *sched = system_scheduler();
    if (sched)
        scheduler_stop(sched);
}

// Interrupt from JS
EMSCRIPTEN_KEEPALIVE void shell_interrupt(void) {
    sigint_handler(SIGINT);
}

// Public interrupt wrapper
void em_main_interrupt(void) {
    shell_interrupt();
}

// ============================================================================
// Filesystem Commands
// ============================================================================

// Track pending sync operations
static volatile int g_sync_pending = 0;

// Callback from JS when sync completes
EMSCRIPTEN_KEEPALIVE void sync_complete(int err) {
    g_sync_pending = 0;
    if (err) {
        printf("[sync] completed with error\n");
    }
}

// Blocking sync: persist memfs changes to IndexedDB, wait for completion
// clang-format off
EM_ASYNC_JS(int, sync_and_wait, (), {
    if (typeof FS === 'undefined' || !FS.syncfs)
        return 0;
    return await new Promise(function(resolve) {
        FS.syncfs(
            false, function(err) {
                if (err) {
                    console.error('[sync] Error:', err);
                    resolve(-1);
                } else {
                    resolve(0);
                }
            });
    });
});
// clang-format on

// Blocking sync: load IndexedDB contents into memfs, wait for completion
// clang-format off
EM_ASYNC_JS(int, sync_from_persist_and_wait, (), {
    if (typeof FS === 'undefined' || !FS.syncfs)
        return 0;
    return await new Promise(function(resolve) {
        FS.syncfs(
            true, function(err) {
                if (err) {
                    console.error('[sync] Error:', err);
                    resolve(-1);
                } else {
                    resolve(0);
                }
            });
    });
});
// clang-format on

// Sync command - sync persistent filesystem
// Usage: sync [wait|status|from-persist [wait]]
//   sync               - trigger a filesystem sync (non-blocking)
//   sync wait          - sync and block until complete
//   sync from-persist  - load from IndexedDB (non-blocking)
//   sync from-persist wait - load from IndexedDB and block until complete
//   sync status        - return 1 if sync pending, 0 if complete
static uint64_t cmd_sync(int argc, char *argv[]) {
    // Check for status query
    if (argc >= 2 && strcmp(argv[1], "status") == 0) {
        int pending = g_sync_pending;
        printf("Sync status: %s\n", pending ? "pending" : "complete");
        return (uint64_t)pending;
    }

    // Check for from-persist variant
    if (argc >= 2 && strcmp(argv[1], "from-persist") == 0) {
        bool wait = (argc >= 3 && strcmp(argv[2], "wait") == 0);
        if (wait) {
            printf("Syncing from persistent storage (blocking)...\n");
            int rc = sync_from_persist_and_wait();
            printf("Sync from persist %s.\n", rc == 0 ? "complete" : "failed");
            return (uint64_t)rc;
        }
        // Non-blocking from-persist
        printf("Syncing from persistent storage...\n");
        // clang-format off
        EM_ASM({ FS.syncfs(true, function(err) { Module._sync_complete(err ? 1 : 0); }); });
        // clang-format on
        printf("Sync from persist requested.\n");
        return 0;
    }

    // Check for blocking wait
    if (argc >= 2 && strcmp(argv[1], "wait") == 0) {
        printf("Syncing persistent filesystem (blocking)...\n");
        int rc = sync_and_wait();
        printf("Sync %s.\n", rc == 0 ? "complete" : "failed");
        return (uint64_t)rc;
    }

    // Default: non-blocking sync (existing behavior)
    g_sync_pending = 1;
    printf("Syncing persistent filesystem...\n");
    // clang-format off
    EM_ASM({ FS.syncfs(false, function(err) { Module._sync_complete(err ? 1 : 0); }); });
    // clang-format on
    printf("Sync requested.\n");
    return 0;
}

// Download command - save file to browser
static uint64_t cmd_download(int argc, char *argv[]) {
    if (argc != 2) {
        printf("usage: download <path>\n");
        return 0;
    }

    const char *path = argv[1];
    struct stat st;
    if (stat(path, &st) != 0) {
        printf("download: cannot access '%s': %s\n", path, strerror(errno));
        return 0;
    }
    if (!S_ISREG(st.st_mode)) {
        printf("download: '%s' is not a regular file\n", path);
        return 0;
    }

    // Perform the download on the JS side
    // clang-format off
    EM_ASM(
        {
            try {
                var path = UTF8ToString($0);
                var FS = Module && Module.FS;
                if (!FS) {
                    console.error('FS not available');
                    return;
                }
                var data = FS.readFile(path);
                var name = path.split('/').filter(Boolean).pop() || 'download.bin';
                var blob = new Blob([data], {type: 'application/octet-stream'});
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
                console.error('download failed', e);
            }
        },
        path);
    // clang-format on

    printf("download: requested '%s'\n", path);
    return 0;
}

// ============================================================================
// Background Checkpoint System
// ============================================================================

#define BACKGROUND_CHECKPOINT_DIR             "/persist/checkpoint"
#define BACKGROUND_CHECKPOINT_SUFFIX          ".checkpoint"
#define BACKGROUND_CHECKPOINT_PENDING_SUFFIX  ".pending"
#define BACKGROUND_CHECKPOINT_COMPLETE_SUFFIX ".complete"
#define BACKGROUND_CHECKPOINT_DIGITS          7
#define BACKGROUND_CHECKPOINT_PATH_MAX        512
#define BACKGROUND_CHECKPOINT_MIN_INTERVAL_MS 750.0

static double g_last_background_checkpoint_ms = 0.0;
static bool g_background_handlers_installed = false;
static bool g_background_checkpoint_inflight = false;
static uint64_t g_pending_checkpoint_seq = 0; // Sequence number of checkpoint awaiting sync confirmation

// Ensure checkpoint directory exists
static int ensure_checkpoint_directory(void) {
    if (mkdir("/persist", 0777) != 0 && errno != EEXIST) {
        if (errno != EISDIR)
            return GS_ERROR;
    }
    struct stat st;
    if (stat(BACKGROUND_CHECKPOINT_DIR, &st) == 0) {
        return S_ISDIR(st.st_mode) ? GS_SUCCESS : GS_ERROR;
    }
    if (errno != ENOENT && errno != 0)
        return GS_ERROR;
    if (mkdir(BACKGROUND_CHECKPOINT_DIR, 0777) != 0 && errno != EEXIST)
        return GS_ERROR;
    return GS_SUCCESS;
}

// Scan checkpoint directory for highest sequence number (considering only complete checkpoints)
static int scan_checkpoint_directory(uint64_t *out_max) {
    if (ensure_checkpoint_directory() != GS_SUCCESS)
        return GS_ERROR;
    DIR *dir = opendir(BACKGROUND_CHECKPOINT_DIR);
    if (!dir)
        return GS_ERROR;
    uint64_t max_seq = 0;
    struct dirent *entry;
    const size_t suffix_len = strlen(BACKGROUND_CHECKPOINT_SUFFIX);
    const size_t complete_suffix_len = strlen(BACKGROUND_CHECKPOINT_COMPLETE_SUFFIX);
    while ((entry = readdir(dir)) != NULL) {
        const char *name = entry->d_name;
        if (!name || name[0] == '.')
            continue;
        size_t len = strlen(name);
        // Look for .complete marker files to find confirmed checkpoints
        if (len > complete_suffix_len &&
            strcmp(name + len - complete_suffix_len, BACKGROUND_CHECKPOINT_COMPLETE_SUFFIX) == 0) {
            // Extract sequence number from marker filename (NNNNNNN.complete)
            bool numeric = true;
            size_t num_len = len - complete_suffix_len;
            for (size_t i = 0; i < num_len; ++i) {
                if (name[i] < '0' || name[i] > '9') {
                    numeric = false;
                    break;
                }
            }
            if (!numeric)
                continue;
            uint64_t value = strtoull(name, NULL, 10);
            if (value > max_seq)
                max_seq = value;
            continue;
        }
        // Also check raw checkpoint files (for backward compatibility during transition)
        if (len <= suffix_len)
            continue;
        if (strcmp(name + len - suffix_len, BACKGROUND_CHECKPOINT_SUFFIX) != 0)
            continue;
        bool numeric = true;
        for (size_t i = 0; i < len - suffix_len; ++i) {
            if (name[i] < '0' || name[i] > '9') {
                numeric = false;
                break;
            }
        }
        if (!numeric)
            continue;
        uint64_t value = strtoull(name, NULL, 10);
        if (value > max_seq)
            max_seq = value;
    }
    closedir(dir);
    if (out_max)
        *out_max = max_seq;
    return GS_SUCCESS;
}

// Build checkpoint path from sequence number
static int build_checkpoint_path(uint64_t seq, char *out_path, size_t out_len) {
    int written = snprintf(out_path, out_len, "%s/%0*llu%s", BACKGROUND_CHECKPOINT_DIR, BACKGROUND_CHECKPOINT_DIGITS,
                           (unsigned long long)seq, BACKGROUND_CHECKPOINT_SUFFIX);
    return (written > 0 && (size_t)written < out_len) ? GS_SUCCESS : GS_ERROR;
}

// Build .pending marker path from sequence number
static int build_pending_path(uint64_t seq, char *out_path, size_t out_len) {
    int written = snprintf(out_path, out_len, "%s/%0*llu%s", BACKGROUND_CHECKPOINT_DIR, BACKGROUND_CHECKPOINT_DIGITS,
                           (unsigned long long)seq, BACKGROUND_CHECKPOINT_PENDING_SUFFIX);
    return (written > 0 && (size_t)written < out_len) ? GS_SUCCESS : GS_ERROR;
}

// Build .complete marker path from sequence number
static int build_complete_path(uint64_t seq, char *out_path, size_t out_len) {
    int written = snprintf(out_path, out_len, "%s/%0*llu%s", BACKGROUND_CHECKPOINT_DIR, BACKGROUND_CHECKPOINT_DIGITS,
                           (unsigned long long)seq, BACKGROUND_CHECKPOINT_COMPLETE_SUFFIX);
    return (written > 0 && (size_t)written < out_len) ? GS_SUCCESS : GS_ERROR;
}

// Find the path to the latest valid (complete) background checkpoint.
// Overrides the weak default in system.c for the WASM platform.
// Returns a static buffer with the path, or NULL if none found.
const char *find_valid_checkpoint_path(void) {
    static char path_buf[BACKGROUND_CHECKPOINT_PATH_MAX];
    uint64_t max_seq = 0;
    if (scan_checkpoint_directory(&max_seq) != GS_SUCCESS || max_seq == 0)
        return NULL;
    if (build_checkpoint_path(max_seq, path_buf, sizeof(path_buf)) != GS_SUCCESS)
        return NULL;
    // Verify the checkpoint file actually exists
    struct stat st;
    if (stat(path_buf, &st) != 0)
        return NULL;
    // Reject checkpoints from a different build (incompatible state layout)
    if (!checkpoint_validate_build_id(path_buf))
        return NULL;
    return path_buf;
}

// Write a small marker file to indicate checkpoint state
static int write_marker_file(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f)
        return GS_ERROR;
    // Write a small marker with timestamp for debugging
    fprintf(f, "%llu\n", (unsigned long long)emscripten_get_now());
    fclose(f);
    return GS_SUCCESS;
}

// Remove a marker file (silent on failure)
static void remove_marker_file(const char *path) {
    unlink(path);
}

// Trigger checkpoint sync to IDBFS with completion callback
// Trigger async sync to persist marker files (called from checkpoint_sync_complete)
static void trigger_marker_sync(uint64_t seq) {
    // clang-format off
    EM_ASM(
        {
            try {
                if (typeof FS !== 'undefined' && FS && FS.syncfs) {
                    var seqNum = $0;
                    FS.syncfs(
                        false, function(err) {
                            if (err) {
                                console.error('[bg-checkpoint] marker sync failed for seq ' + seqNum, err);
                            } else {
                                console.log('[bg-checkpoint] checkpoint ' + seqNum + ' fully persisted');
                            }
                        });
                }
            } catch (e) {
                console.error('[bg-checkpoint] marker sync exception', e);
            }
        },
        (int)seq);
    // clang-format on
}

// Trigger async sync of checkpoint file to IndexedDB
static void trigger_checkpoint_sync(uint64_t seq) {
    // clang-format off
    EM_ASM(
        {
            try {
                if (typeof FS !== 'undefined' && FS && FS.syncfs) {
                    var seqNum = $0;
                    FS.syncfs(
                        false, function(err) {
                            if (err) {
                                console.error('[bg-checkpoint] sync failed for seq ' + seqNum, err);
                            } else {
                                console.log('[bg-checkpoint] sync complete for seq ' + seqNum);
                            }
                            // Notify C side - it will write markers and trigger another sync
                            if (typeof Module._checkpoint_sync_complete === 'function') {
                                Module._checkpoint_sync_complete(BigInt(seqNum), err ? 1 : 0);
                            }
                        });
                }
            } catch (e) {
                console.error('[bg-checkpoint] sync exception', e);
            }
        },
        (int)seq);
    // clang-format on
}

// Save a quick checkpoint
static int save_quick_checkpoint(const char *reason, bool verbose, bool rate_limit) {
    scheduler_t *sched = system_scheduler();
    if (!sched)
        return GS_ERROR;

    double now = emscripten_get_now();

    if (rate_limit && g_last_background_checkpoint_ms > 0.0) {
        double delta = now - g_last_background_checkpoint_ms;
        if (delta >= 0.0 && delta < BACKGROUND_CHECKPOINT_MIN_INTERVAL_MS)
            return GS_SUCCESS;
    }

    if (g_background_checkpoint_inflight)
        return GS_ERROR;
    if (ensure_checkpoint_directory() != GS_SUCCESS)
        return GS_ERROR;

    uint64_t max_seq = 0;
    if (scan_checkpoint_directory(&max_seq) != GS_SUCCESS)
        return GS_ERROR;

    uint64_t next_seq = max_seq + 1;
    char path[BACKGROUND_CHECKPOINT_PATH_MAX];
    char pending_path[BACKGROUND_CHECKPOINT_PATH_MAX];
    if (build_checkpoint_path(next_seq, path, sizeof(path)) != GS_SUCCESS)
        return GS_ERROR;
    if (build_pending_path(next_seq, pending_path, sizeof(pending_path)) != GS_SUCCESS)
        return GS_ERROR;

    // Create .pending marker BEFORE writing checkpoint data
    // This signals that a checkpoint write is in progress
    if (write_marker_file(pending_path) != GS_SUCCESS) {
        printf("[checkpoint] failed to create pending marker\n");
        return GS_ERROR;
    }

    // Record running state before stopping - this will be saved in the checkpoint
    bool was_running = scheduler_is_running(sched);
    if (was_running)
        scheduler_stop(sched);

    // Temporarily restore running flag so checkpoint captures the pre-stop state
    // This allows load-state to auto-resume if the checkpoint was saved while running
    if (was_running && sched)
        scheduler_set_running(sched, true);

    // Start timing the checkpoint save operation
    double checkpoint_start_time = emscripten_get_now();

    g_background_checkpoint_inflight = true;
    g_pending_checkpoint_seq = next_seq;
    int rc = setup_plus_checkpoint(path, CHECKPOINT_KIND_QUICK);

    // Calculate elapsed time for checkpoint save
    double checkpoint_elapsed_ms = emscripten_get_now() - checkpoint_start_time;

    // Note: running state is already set correctly from the pre-checkpoint restore

    if (rc == GS_SUCCESS) {
        g_last_background_checkpoint_ms = now;
        if (verbose) {
            printf("[checkpoint] quick checkpoint saved to %s (%s, %.2f ms), awaiting sync\n", path,
                   reason ? reason : "background", checkpoint_elapsed_ms);
        }
        // Trigger async sync - completion callback will mark as complete and clean up
        trigger_checkpoint_sync(next_seq);
        // Note: g_background_checkpoint_inflight stays true until sync completes
    } else {
        if (verbose) {
            printf("[checkpoint] quick checkpoint failed (%s)\n", reason ? reason : "background");
        }
        // Clean up pending marker on failure
        remove_marker_file(pending_path);
        g_background_checkpoint_inflight = false;
        g_pending_checkpoint_seq = 0;
    }
    return rc;
}

// Callback when IndexedDB sync completes (called from JS)
// Writes completion marker, cleans up old checkpoints, triggers marker sync
EMSCRIPTEN_KEEPALIVE void checkpoint_sync_complete(uint64_t seq, int error) {
    char pending_path[BACKGROUND_CHECKPOINT_PATH_MAX];
    char complete_path[BACKGROUND_CHECKPOINT_PATH_MAX];
    char checkpoint_path[BACKGROUND_CHECKPOINT_PATH_MAX];

    if (build_pending_path(seq, pending_path, sizeof(pending_path)) != GS_SUCCESS ||
        build_complete_path(seq, complete_path, sizeof(complete_path)) != GS_SUCCESS ||
        build_checkpoint_path(seq, checkpoint_path, sizeof(checkpoint_path)) != GS_SUCCESS) {
        g_background_checkpoint_inflight = false;
        g_pending_checkpoint_seq = 0;
        return;
    }

    if (error) {
        printf("[checkpoint] sync failed for seq %llu, checkpoint incomplete\n", (unsigned long long)seq);
        // Leave .pending marker in place to indicate incomplete checkpoint
        g_background_checkpoint_inflight = false;
        g_pending_checkpoint_seq = 0;
        return;
    }

    // Sync succeeded - write completion marker and clean up
    // Remove .pending marker first
    remove_marker_file(pending_path);

    // Write .complete marker
    if (write_marker_file(complete_path) != GS_SUCCESS) {
        printf("[checkpoint] failed to create complete marker for seq %llu\n", (unsigned long long)seq);
        g_background_checkpoint_inflight = false;
        g_pending_checkpoint_seq = 0;
        return;
    }

    printf("[checkpoint] checkpoint %llu marked complete\n", (unsigned long long)seq);

    // Clean up old checkpoints (keep only this one)
    DIR *dir = opendir(BACKGROUND_CHECKPOINT_DIR);
    if (dir) {
        struct dirent *entry;
        const size_t complete_suffix_len = strlen(BACKGROUND_CHECKPOINT_COMPLETE_SUFFIX);
        const size_t checkpoint_suffix_len = strlen(BACKGROUND_CHECKPOINT_SUFFIX);
        const size_t pending_suffix_len = strlen(BACKGROUND_CHECKPOINT_PENDING_SUFFIX);

        while ((entry = readdir(dir)) != NULL) {
            const char *name = entry->d_name;
            if (!name || name[0] == '.')
                continue;

            size_t len = strlen(name);
            uint64_t file_seq = 0;
            bool is_checkpoint = false;
            bool is_complete = false;
            bool is_pending = false;

            // Check for .complete marker
            if (len > complete_suffix_len &&
                strcmp(name + len - complete_suffix_len, BACKGROUND_CHECKPOINT_COMPLETE_SUFFIX) == 0) {
                is_complete = true;
                file_seq = strtoull(name, NULL, 10);
            }
            // Check for .pending marker
            else if (len > pending_suffix_len &&
                     strcmp(name + len - pending_suffix_len, BACKGROUND_CHECKPOINT_PENDING_SUFFIX) == 0) {
                is_pending = true;
                file_seq = strtoull(name, NULL, 10);
            }
            // Check for .checkpoint file
            else if (len > checkpoint_suffix_len &&
                     strcmp(name + len - checkpoint_suffix_len, BACKGROUND_CHECKPOINT_SUFFIX) == 0) {
                is_checkpoint = true;
                file_seq = strtoull(name, NULL, 10);
            }

            if (file_seq == 0)
                continue;

            // Delete old files (seq < current)
            if (file_seq < seq) {
                char old_path[BACKGROUND_CHECKPOINT_PATH_MAX];
                snprintf(old_path, sizeof(old_path), "%s/%s", BACKGROUND_CHECKPOINT_DIR, name);
                if (unlink(old_path) == 0) {
                    printf("[checkpoint] cleaned up old %s\n", name);
                }
            }
        }
        closedir(dir);
    }

    // Reset inflight state
    if (g_pending_checkpoint_seq == seq) {
        g_background_checkpoint_inflight = false;
        g_pending_checkpoint_seq = 0;
    }

    // Trigger another sync to persist the .complete marker and cleanup
    trigger_marker_sync(seq);
}

// Request background checkpoint (with rate limiting)
static void maybe_request_background_checkpoint(const char *reason, bool rate_limit) {
    int rc = save_quick_checkpoint(reason, false, rate_limit);
    // clang-format off
    EM_ASM({ Module.__gsLastBackgroundCheckpointRC = $0; }, rc);
    // clang-format on
    if (rc != GS_SUCCESS) {
        printf("[checkpoint] background checkpoint failed (%s)\n", reason ? reason : "background");
    }
}

// Install pagehide hook (JS)
// clang-format off
EM_JS(void, install_pagehide_hook, (), {
    if (typeof Module === 'undefined')
        return;
    if (Module.__gsBackgroundPagehideHookInstalled)
        return;
    Module.__gsBackgroundPagehideHookInstalled = true;

    function invoke_checkpoint_from_js() {
        try {
            console.log('[bg-checkpoint] pagehide hook invoked');
        } catch (_) {
        }
        if (typeof Module === 'undefined')
            return;
        if (typeof Module._background_checkpoint_from_js === 'function') {
            Module._background_checkpoint_from_js();
            try {
                console.log('[bg-checkpoint] rc:', Module.__gsLastBackgroundCheckpointRC);
            } catch (_) {
            }
            return;
        }
        if (typeof Module.cwrap === 'function') {
            Module._background_checkpoint_from_js = Module.cwrap("background_checkpoint_from_js", "void", []);
            Module._background_checkpoint_from_js();
            try {
                console.log('[bg-checkpoint] rc:', Module.__gsLastBackgroundCheckpointRC);
            } catch (_) {
            }
        }
    }

    addEventListener('pagehide', invoke_checkpoint_from_js);
    document.addEventListener('visibilitychange', function() {
        if (document.hidden)
            invoke_checkpoint_from_js();
    });
});
// clang-format on

// Checkpoint from JS callback
EMSCRIPTEN_KEEPALIVE void background_checkpoint_from_js(void) {
    maybe_request_background_checkpoint("pagehide", true);
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
    install_pagehide_hook();
    g_background_handlers_installed = true;
}

// Background checkpoint command
static uint64_t cmd_background_checkpoint(int argc, char *argv[]) {
    const char *reason = (argc >= 2) ? argv[1] : "manual";
    int rc = save_quick_checkpoint(reason, true, false);
    return (rc == GS_SUCCESS) ? 0 : -1;
}

// Clear all checkpoint files from the checkpoint directory
static int clear_checkpoint_files(void) {
    DIR *dir = opendir(BACKGROUND_CHECKPOINT_DIR);
    if (!dir)
        return 0; // Nothing to clear

    struct dirent *entry;
    int removed = 0;
    char path[BACKGROUND_CHECKPOINT_PATH_MAX];

    while ((entry = readdir(dir)) != NULL) {
        const char *name = entry->d_name;
        if (!name || name[0] == '.')
            continue;

        size_t len = strlen(name);
        // Remove files ending in .checkpoint, .complete, or .pending
        bool is_checkpoint =
            len > strlen(BACKGROUND_CHECKPOINT_SUFFIX) &&
            strcmp(name + len - strlen(BACKGROUND_CHECKPOINT_SUFFIX), BACKGROUND_CHECKPOINT_SUFFIX) == 0;
        bool is_complete = len > strlen(BACKGROUND_CHECKPOINT_COMPLETE_SUFFIX) &&
                           strcmp(name + len - strlen(BACKGROUND_CHECKPOINT_COMPLETE_SUFFIX),
                                  BACKGROUND_CHECKPOINT_COMPLETE_SUFFIX) == 0;
        bool is_pending = len > strlen(BACKGROUND_CHECKPOINT_PENDING_SUFFIX) &&
                          strcmp(name + len - strlen(BACKGROUND_CHECKPOINT_PENDING_SUFFIX),
                                 BACKGROUND_CHECKPOINT_PENDING_SUFFIX) == 0;

        if (is_checkpoint || is_complete || is_pending) {
            snprintf(path, sizeof(path), "%s/%s", BACKGROUND_CHECKPOINT_DIR, name);
            if (unlink(path) == 0)
                removed++;
        }
    }
    closedir(dir);
    return removed;
}

// Checkpoint command dispatcher - handles auto on/off and clear
static uint64_t cmd_checkpoint(int argc, char *argv[]) {
    if (argc < 2) {
        // No arguments - just query and return current auto state
        printf("Current state: %s\n", checkpoint_auto_enabled ? "on" : "off");
        return 0;
    }

    const char *action = argv[1];

    // checkpoint clear - remove all checkpoint files
    if (strcmp(action, "clear") == 0) {
        int removed = clear_checkpoint_files();
        printf("Cleared %d checkpoint file(s)\n", removed);
        return 0;
    }

    // checkpoint auto on/off
    if (strcmp(action, "auto") == 0) {
        if (argc < 3) {
            printf("Current state: %s\n", checkpoint_auto_enabled ? "on" : "off");
            return 0;
        }
        const char *toggle = argv[2];
        if (strcmp(toggle, "off") == 0 || strcmp(toggle, "disable") == 0) {
            checkpoint_auto_enabled = false;
            checkpoint_tick_counter = 0;
            printf("Automatic checkpointing disabled\n");
            return 0;
        } else if (strcmp(toggle, "on") == 0 || strcmp(toggle, "enable") == 0) {
            checkpoint_auto_enabled = true;
            printf("Automatic checkpointing enabled\n");
            return 0;
        }
        printf("Usage: checkpoint auto <on|off>\n");
        return 1;
    }

    // Legacy: checkpoint on/off (without 'auto' subcommand)
    if (strcmp(action, "off") == 0 || strcmp(action, "disable") == 0) {
        checkpoint_auto_enabled = false;
        checkpoint_tick_counter = 0;
        printf("Automatic checkpointing disabled\n");
        return 0;
    } else if (strcmp(action, "on") == 0 || strcmp(action, "enable") == 0) {
        checkpoint_auto_enabled = true;
        printf("Automatic checkpointing enabled\n");
        return 0;
    }

    printf("Usage: checkpoint <clear|auto <on|off>|on|off>\n");
    return 1;
}

// ============================================================================
// Exported Runtime Query Functions (for tests and diagnostics)
// ============================================================================

// Direct mouse event handler for tests (bypasses HTML5 pointer lock)
// Used by e2e tests to inject synthetic mouse button state
EMSCRIPTEN_KEEPALIVE
void handle_mouse_event(int button_state, int dx, int dy) {
    system_mouse_update(button_state != 0, dx, dy);
}

// ============================================================================
// Main Entry Point
// ============================================================================

// Forward declaration of external command registration
extern void em_audio_register_commands(void);

int main(int argc, char *argv[]) {
    signal(SIGINT, sigint_handler);

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
    global_emulator = setup_plus(NULL); // NULL = setup from scratch, not from checkpoint

    // Apply scheduler speed mode if provided
    scheduler_t *sched = system_scheduler();
    if (speed_mode && sched) {
        if (strcmp(speed_mode, "max") == 0) {
            scheduler_set_mode(sched, schedule_max_speed);
            printf("[C] Scheduler mode set via --speed=max\n");
        } else if (strcmp(speed_mode, "realtime") == 0 || strcmp(speed_mode, "real") == 0) {
            scheduler_set_mode(sched, schedule_real_time);
            printf("[C] Scheduler mode set via --speed=realtime\n");
        } else if (strcmp(speed_mode, "hardware") == 0 || strcmp(speed_mode, "hw") == 0 ||
                   strcmp(speed_mode, "accuracy") == 0) {
            scheduler_set_mode(sched, schedule_hw_accuracy);
            printf("[C] Scheduler mode set via --speed=hardware\n");
        } else {
            printf("[C] Unknown --speed mode '%s' (valid: max|realtime|hardware)\n", speed_mode);
        }
    }

    printf("Macintosh Plus (4 MB RAM, 7.8 MHz)\n");

    // Initialize subsystems
    em_video_init();
    em_audio_init();
    setup_pointer_lock();

    // Register shell commands
    register_cmd("sync", "Filesystem", "sync [wait|from-persist [wait]|status] – sync persistent filesystem", cmd_sync);
    register_cmd("download", "Filesystem", "download <path> – save file to your computer", cmd_download);
    register_cmd("background-checkpoint", "Checkpointing",
                 "background-checkpoint [reason] – save a quick checkpoint to /persist/checkpoint",
                 cmd_background_checkpoint);
    register_cmd("checkpoint", "Checkpointing", "checkpoint <clear|auto <on|off>|on|off> – manage checkpoints",
                 cmd_checkpoint);
    em_audio_register_commands();

    install_background_checkpoint_handlers();

    // Register platform-specific assertion callback
    debug_t *debug = system_debug();
    if (debug) {
        debug->assertion_callback = em_assertion_callback;
    }

    emscripten_set_main_loop(tick, 0, 1); // Use RAF, simulate infinite loop
    return 0;
}

// ============================================================================
// Shell Command Interface for JavaScript
// ============================================================================

// Platform wrapper for shell command handling; exposed to JavaScript
EMSCRIPTEN_KEEPALIVE
uint64_t em_handle_command(const char *input_line) {
    return handle_command(input_line);
}

// ============================================================================
// Assertion Notification for JavaScript
// ============================================================================

// JavaScript callback to notify the browser that an assertion has failed
// This allows Playwright tests to immediately detect and fail on assertions
// clang-format off
EM_JS(void, js_notify_assertion_failure, (const char *expr, const char *file, int line, const char *func), {
    const exprStr = expr ? UTF8ToString(expr) : "";
    const fileStr = file ? UTF8ToString(file) : "<unknown>";
    const funcStr = func ? UTF8ToString(func) : "<unknown>";

    // Call global handler if registered
    if (typeof window !== 'undefined' && typeof window.__gsAssertionHandler === 'function') {
        try {
            window.__gsAssertionHandler(
                {expr: exprStr, file: fileStr, line: line, func: funcStr, timestamp: Date.now()});
        } catch (e) {
            // Handler may throw to stop execution - this is expected
        }
    }
});
// clang-format on

// Platform-specific assertion callback implementation
static void em_assertion_callback(const char *expr, const char *file, int line, const char *func) {
    js_notify_assertion_failure(expr, file, line, func);
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
