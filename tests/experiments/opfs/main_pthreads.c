// OPFS experiment program — pthreads variant
// Build: emcc main_pthreads.c -sWASMFS -sFORCE_FILESYSTEM -lopfs.js -pthread -sPROXY_TO_PTHREAD ...

#include <dirent.h>
#include <emscripten/emscripten.h>
#include <emscripten/threading.h>
#include <emscripten/wasmfs.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// ============================================================================
// Experiment 1: POSIX I/O on OPFS
// ============================================================================
static int exp_filesystem_setup(void) {
    printf("[exp1] Testing POSIX I/O on OPFS (pthreads)...\n");
    printf("[exp1] Running on main browser thread: %s\n", emscripten_is_main_browser_thread() ? "YES" : "no (worker)");

    FILE *f = fopen("/opfs/testfile.txt", "w");
    if (!f) {
        printf("[exp1] FAIL: fopen: %s\n", strerror(errno));
        return 1;
    }
    fprintf(f, "hello from opfs pthreads");
    fclose(f);

    f = fopen("/opfs/testfile.txt", "r");
    if (!f) {
        printf("[exp1] FAIL: fopen read: %s\n", strerror(errno));
        return 1;
    }
    char buf[64] = {0};
    fgets(buf, sizeof(buf), f);
    fclose(f);
    if (strcmp(buf, "hello from opfs pthreads") != 0) {
        printf("[exp1] FAIL: read back '%s'\n", buf);
        return 1;
    }

    if (mkdir("/opfs/testdir", 0777) != 0 && errno != EEXIST) {
        printf("[exp1] FAIL: mkdir: %s\n", strerror(errno));
        return 1;
    }
    f = fopen("/opfs/testdir/nested.txt", "w");
    if (!f) {
        printf("[exp1] FAIL: fopen nested: %s\n", strerror(errno));
        return 1;
    }
    fprintf(f, "nested");
    fclose(f);

    DIR *d = opendir("/opfs/testdir");
    if (!d) {
        printf("[exp1] FAIL: opendir: %s\n", strerror(errno));
        return 1;
    }
    int found = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, "nested.txt") == 0)
            found = 1;
    }
    closedir(d);
    if (!found) {
        printf("[exp1] FAIL: nested.txt not in readdir\n");
        return 1;
    }

    struct stat st;
    if (stat("/opfs/testfile.txt", &st) != 0) {
        printf("[exp1] FAIL: stat: %s\n", strerror(errno));
        return 1;
    }
    printf("[exp1] stat size=%ld\n", (long)st.st_size);

    if (unlink("/opfs/testfile.txt") != 0) {
        printf("[exp1] FAIL: unlink: %s\n", strerror(errno));
        return 1;
    }

    unlink("/opfs/testdir/nested.txt");
    rmdir("/opfs/testdir");
    printf("[exp1] PASS\n");
    return 0;
}

// ============================================================================
// Experiment 2: fseek/fwrite granularity
// ============================================================================
static int exp_seek_write(void) {
    printf("[exp2] Testing fseek/fwrite granularity (pthreads)...\n");

    FILE *f = fopen("/opfs/seektest.bin", "w+b");
    if (!f) {
        printf("[exp2] FAIL: fopen: %s\n", strerror(errno));
        return 1;
    }

    uint8_t pat_a[512];
    memset(pat_a, 0xAA, 512);
    uint8_t pat_b[512];
    memset(pat_b, 0xBB, 512);
    uint8_t pat_c[512];
    memset(pat_c, 0xCC, 512);

    fseek(f, 1024, SEEK_SET);
    fwrite(pat_a, 512, 1, f);
    fseek(f, 0, SEEK_SET);
    fwrite(pat_b, 512, 1, f);
    fseek(f, 4096, SEEK_SET);
    fwrite(pat_c, 512, 1, f);

    uint8_t rd[512];
    // Verify offset 0
    fseek(f, 0, SEEK_SET);
    fread(rd, 512, 1, f);
    for (int i = 0; i < 512; i++)
        if (rd[i] != 0xBB) {
            printf("[exp2] FAIL: off=0 byte %d\n", i);
            fclose(f);
            return 1;
        }
    // Verify gap at 512
    fseek(f, 512, SEEK_SET);
    fread(rd, 512, 1, f);
    for (int i = 0; i < 512; i++)
        if (rd[i] != 0x00) {
            printf("[exp2] FAIL: gap byte %d\n", i);
            fclose(f);
            return 1;
        }
    // Verify offset 1024
    fseek(f, 1024, SEEK_SET);
    fread(rd, 512, 1, f);
    for (int i = 0; i < 512; i++)
        if (rd[i] != 0xAA) {
            printf("[exp2] FAIL: off=1024 byte %d\n", i);
            fclose(f);
            return 1;
        }
    // Verify offset 4096
    fseek(f, 4096, SEEK_SET);
    fread(rd, 512, 1, f);
    for (int i = 0; i < 512; i++)
        if (rd[i] != 0xCC) {
            printf("[exp2] FAIL: off=4096 byte %d\n", i);
            fclose(f);
            return 1;
        }

    fclose(f);
    unlink("/opfs/seektest.bin");
    printf("[exp2] PASS\n");
    return 0;
}

// ============================================================================
// Experiment 3: Performance (open-handle vs reopen)
// ============================================================================
static int exp_performance(void) {
    printf("[exp3] Testing SyncAccessHandle performance (pthreads)...\n");

    const int ITERATIONS = 500;
    uint8_t block[512];
    memset(block, 0x42, 512);

    // Test A: Keep handle open
    FILE *f = fopen("/opfs/perftest.bin", "w+b");
    if (!f) {
        printf("[exp3] FAIL: fopen\n");
        return 1;
    }
    double t0 = emscripten_get_now();
    for (int i = 0; i < ITERATIONS; i++) {
        fseek(f, (size_t)(i % 100) * 512, SEEK_SET);
        fwrite(block, 512, 1, f);
    }
    double t1 = emscripten_get_now();
    fclose(f);
    double open_ms = t1 - t0;

    // Test B: Reopen each time
    f = fopen("/opfs/perftest2.bin", "wb");
    for (int i = 0; i < 100; i++)
        fwrite(block, 512, 1, f);
    fclose(f);

    t0 = emscripten_get_now();
    for (int i = 0; i < ITERATIONS; i++) {
        f = fopen("/opfs/perftest2.bin", "r+b");
        if (!f) {
            printf("[exp3] FAIL: reopen %d\n", i);
            return 1;
        }
        fseek(f, (size_t)(i % 100) * 512, SEEK_SET);
        fwrite(block, 512, 1, f);
        fclose(f);
    }
    t1 = emscripten_get_now();
    double reopen_ms = t1 - t0;

    printf("[exp3] open-handle: %.1f ms (%d writes, %.3f ms/write)\n", open_ms, ITERATIONS, open_ms / ITERATIONS);
    printf("[exp3] reopen-each: %.1f ms (%d writes, %.3f ms/write)\n", reopen_ms, ITERATIONS, reopen_ms / ITERATIONS);
    printf("[exp3] ratio: %.1fx\n", reopen_ms / (open_ms > 0 ? open_ms : 0.001));

    unlink("/opfs/perftest.bin");
    unlink("/opfs/perftest2.bin");
    printf("[exp3] PASS\n");
    return 0;
}

// ============================================================================
// Experiment 4: Persistence (write marker for reload test)
// ============================================================================
static int exp_persistence_write(void) {
    printf("[exp4] Writing persistence marker...\n");
    if (mkdir("/opfs/persist_test", 0777) != 0 && errno != EEXIST) {
        printf("[exp4] FAIL: mkdir: %s\n", strerror(errno));
        return 1;
    }
    FILE *f = fopen("/opfs/persist_test/marker.txt", "w");
    if (!f) {
        printf("[exp4] FAIL: fopen: %s\n", strerror(errno));
        return 1;
    }
    fprintf(f, "PERSIST_PTHREADS_OK");
    fclose(f);
    printf("[exp4] PASS (marker written)\n");
    return 0;
}

static int exp_persistence_check(void) {
    printf("[exp4b] Checking persistence marker...\n");
    FILE *f = fopen("/opfs/persist_test/marker.txt", "r");
    if (!f) {
        printf("[exp4b] FAIL: marker not found: %s\n", strerror(errno));
        return 1;
    }
    char buf[64] = {0};
    fgets(buf, sizeof(buf), f);
    fclose(f);
    if (strcmp(buf, "PERSIST_PTHREADS_OK") != 0) {
        printf("[exp4b] FAIL: content='%s'\n", buf);
        return 1;
    }
    printf("[exp4b] PASS\n");
    return 0;
}

static int exp_persistence_cleanup(void) {
    unlink("/opfs/persist_test/marker.txt");
    rmdir("/opfs/persist_test");
    printf("[exp4c] cleanup done\n");
    return 0;
}

// ============================================================================
// Experiment 5: main_loop with file I/O
// ============================================================================
static int g_tick_count = 0;
static int g_tick_ok = 1;

static void tick_with_io(void) {
    g_tick_count++;
    if (g_tick_count <= 3) {
        char path[64];
        snprintf(path, sizeof(path), "/opfs/tick_%d.tmp", g_tick_count);
        FILE *f = fopen(path, "w");
        if (!f) {
            printf("[exp5] FAIL: tick %d fopen: %s\n", g_tick_count, strerror(errno));
            g_tick_ok = 0;
        } else {
            fprintf(f, "tick%d", g_tick_count);
            fclose(f);
        }
    }
    if (g_tick_count >= 5) {
        for (int i = 1; i <= 3; i++) {
            char path[64], expected[16], buf[16] = {0};
            snprintf(path, sizeof(path), "/opfs/tick_%d.tmp", i);
            snprintf(expected, sizeof(expected), "tick%d", i);
            FILE *f = fopen(path, "r");
            if (!f) {
                printf("[exp5] FAIL: tick %d read\n", i);
                g_tick_ok = 0;
                continue;
            }
            fgets(buf, sizeof(buf), f);
            fclose(f);
            if (strcmp(buf, expected) != 0) {
                printf("[exp5] FAIL: tick %d got '%s'\n", i, buf);
                g_tick_ok = 0;
            }
            unlink(path);
        }
        printf("[exp5] %s\n", g_tick_ok ? "PASS" : "FAIL");
        emscripten_cancel_main_loop();
    }
}

static int exp_main_loop_io(void) {
    printf("[exp5] Testing main_loop + OPFS I/O (pthreads)...\n");
    g_tick_count = 0;
    g_tick_ok = 1;
    emscripten_set_main_loop(tick_with_io, 0, 0);
    return 0;
}

// ============================================================================
// Experiment 6: EM_ASM from worker thread
// ============================================================================
static int exp_em_asm_from_worker(void) {
    printf("[exp6] Testing EM_ASM from worker thread...\n");
    printf("[exp6] Is main browser thread: %s\n", emscripten_is_main_browser_thread() ? "YES" : "no (worker)");

    // This should work — simple console.log doesn't need DOM
    EM_ASM({ console.log('[exp6] EM_ASM from worker: console.log works'); });

    // This would fail on worker — test if we can detect it
    int is_main = EM_ASM_INT({ return typeof document != = 'undefined' ? 1 : 0; });
    printf("[exp6] document available: %s\n", is_main ? "yes (main thread)" : "no (worker)");

    printf("[exp6] PASS\n");
    return 0;
}

// ============================================================================
// Dispatch
// ============================================================================
EMSCRIPTEN_KEEPALIVE
int run_experiment(const char *name) {
    if (strcmp(name, "filesystem_setup") == 0)
        return exp_filesystem_setup();
    if (strcmp(name, "seek_write") == 0)
        return exp_seek_write();
    if (strcmp(name, "performance") == 0)
        return exp_performance();
    if (strcmp(name, "persistence_write") == 0)
        return exp_persistence_write();
    if (strcmp(name, "persistence_check") == 0)
        return exp_persistence_check();
    if (strcmp(name, "persistence_cleanup") == 0)
        return exp_persistence_cleanup();
    if (strcmp(name, "main_loop_io") == 0)
        return exp_main_loop_io();
    if (strcmp(name, "em_asm_worker") == 0)
        return exp_em_asm_from_worker();
    printf("Unknown experiment: %s\n", name);
    return -1;
}

// ============================================================================
// Main
// ============================================================================
int main(int argc, char *argv[]) {
    printf("main() on main browser thread: %s\n", emscripten_is_main_browser_thread() ? "YES" : "no (worker)");

    backend_t opfs = wasmfs_create_opfs_backend();
    int rc = wasmfs_create_directory("/opfs", 0777, opfs);
    if (rc != 0) {
        printf("FATAL: wasmfs_create_directory /opfs failed (rc=%d)\n", rc);
        return 1;
    }
    printf("Filesystem ready: /opfs=OPFS\n");

    // Run experiment specified by --run=<name>
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--run=", 6) == 0) {
            const char *name = argv[i] + 6;
            printf("Running experiment: %s\n", name);
            int result = run_experiment(name);
            printf("EXIT_CODE:%d\n", result);
            return result;
        }
    }

    printf("No --run= argument. Ready for ccall.\n");
    return 0;
}
