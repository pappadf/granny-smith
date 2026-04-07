// OPFS experiment program — validates WasmFS + OPFS + JSPI assumptions
// Build: emcc main.c -sWASMFS -sASYNCIFY=2 -lopfs.js -sEXPORTED_RUNTIME_METHODS=['FS','ccall']
// -sEXPORTED_FUNCTIONS=['_main','_run_experiment'] -sMODULARIZE -sEXPORT_NAME=createModule -o build/main.mjs

#include <dirent.h>
#include <emscripten/emscripten.h>
#include <emscripten/wasmfs.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// ============================================================================
// Experiment 1: OPFS root + /tmp memory backend
// ============================================================================
static int exp_filesystem_setup(void) {
    printf("[exp1] Testing filesystem setup...\n");

    // Write to OPFS-backed /data path
    FILE *f = fopen("/opfs/testfile.txt", "w");
    if (!f) {
        printf("[exp1] FAIL: fopen /testfile.txt: %s\n", strerror(errno));
        return 1;
    }
    fprintf(f, "hello from opfs");
    fclose(f);

    // Read it back
    f = fopen("/opfs/testfile.txt", "r");
    if (!f) {
        printf("[exp1] FAIL: fopen read: %s\n", strerror(errno));
        return 1;
    }
    char buf[64] = {0};
    fgets(buf, sizeof(buf), f);
    fclose(f);
    if (strcmp(buf, "hello from opfs") != 0) {
        printf("[exp1] FAIL: read back '%s'\n", buf);
        return 1;
    }

    // mkdir on OPFS
    if (mkdir("/opfs/testdir", 0777) != 0 && errno != EEXIST) {
        printf("[exp1] FAIL: mkdir /testdir: %s\n", strerror(errno));
        return 1;
    }

    // Write inside subdir
    f = fopen("/opfs/testdir/nested.txt", "w");
    if (!f) {
        printf("[exp1] FAIL: fopen nested: %s\n", strerror(errno));
        return 1;
    }
    fprintf(f, "nested");
    fclose(f);

    // readdir
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
        printf("[exp1] FAIL: nested.txt not found in readdir\n");
        return 1;
    }

    // stat
    struct stat st;
    if (stat("/opfs/testfile.txt", &st) != 0) {
        printf("[exp1] FAIL: stat: %s\n", strerror(errno));
        return 1;
    }
    printf("[exp1] stat size=%ld\n", (long)st.st_size);

    // unlink
    if (unlink("/opfs/testfile.txt") != 0) {
        printf("[exp1] FAIL: unlink: %s\n", strerror(errno));
        return 1;
    }
    if (stat("/opfs/testfile.txt", &st) == 0) {
        printf("[exp1] FAIL: file still exists after unlink\n");
        return 1;
    }

    // Clean up
    unlink("/opfs/testdir/nested.txt");
    rmdir("/opfs/testdir");
    unlink("/tmp/scratch.txt");

    printf("[exp1] PASS\n");
    return 0;
}

// ============================================================================
// Experiment 2: fseek/fwrite granularity (512-byte blocks at arbitrary offsets)
// ============================================================================
static int exp_seek_write_granularity(void) {
    printf("[exp2] Testing fseek/fwrite granularity...\n");

    FILE *f = fopen("/opfs/seektest.bin", "w+b");
    if (!f) {
        printf("[exp2] FAIL: fopen: %s\n", strerror(errno));
        return 1;
    }

    // Write 512 bytes at offset 1024
    uint8_t pattern_a[512];
    memset(pattern_a, 0xAA, 512);
    fseek(f, 1024, SEEK_SET);
    if (fwrite(pattern_a, 512, 1, f) != 1) {
        printf("[exp2] FAIL: fwrite at 1024\n");
        fclose(f);
        return 1;
    }

    // Write 512 bytes at offset 0
    uint8_t pattern_b[512];
    memset(pattern_b, 0xBB, 512);
    fseek(f, 0, SEEK_SET);
    if (fwrite(pattern_b, 512, 1, f) != 1) {
        printf("[exp2] FAIL: fwrite at 0\n");
        fclose(f);
        return 1;
    }

    // Write 512 bytes at offset 4096
    uint8_t pattern_c[512];
    memset(pattern_c, 0xCC, 512);
    fseek(f, 4096, SEEK_SET);
    if (fwrite(pattern_c, 512, 1, f) != 1) {
        printf("[exp2] FAIL: fwrite at 4096\n");
        fclose(f);
        return 1;
    }

    // Read back and verify all three regions
    uint8_t readbuf[512];

    // Offset 0: should be 0xBB
    fseek(f, 0, SEEK_SET);
    fread(readbuf, 512, 1, f);
    for (int i = 0; i < 512; i++) {
        if (readbuf[i] != 0xBB) {
            printf("[exp2] FAIL: offset 0, byte %d = 0x%02X (expected 0xBB)\n", i, readbuf[i]);
            fclose(f);
            return 1;
        }
    }

    // Offset 512: gap — should be zeros
    fseek(f, 512, SEEK_SET);
    fread(readbuf, 512, 1, f);
    for (int i = 0; i < 512; i++) {
        if (readbuf[i] != 0x00) {
            printf("[exp2] FAIL: gap at 512, byte %d = 0x%02X (expected 0x00)\n", i, readbuf[i]);
            fclose(f);
            return 1;
        }
    }

    // Offset 1024: should be 0xAA
    fseek(f, 1024, SEEK_SET);
    fread(readbuf, 512, 1, f);
    for (int i = 0; i < 512; i++) {
        if (readbuf[i] != 0xAA) {
            printf("[exp2] FAIL: offset 1024, byte %d = 0x%02X (expected 0xAA)\n", i, readbuf[i]);
            fclose(f);
            return 1;
        }
    }

    // Offset 4096: should be 0xCC
    fseek(f, 4096, SEEK_SET);
    fread(readbuf, 512, 1, f);
    for (int i = 0; i < 512; i++) {
        if (readbuf[i] != 0xCC) {
            printf("[exp2] FAIL: offset 4096, byte %d = 0x%02X (expected 0xCC)\n", i, readbuf[i]);
            fclose(f);
            return 1;
        }
    }

    fclose(f);
    unlink("/opfs/seektest.bin");
    printf("[exp2] PASS\n");
    return 0;
}

// ============================================================================
// Experiment 3: Open-handle performance (seek+write vs open+write+close)
// ============================================================================
static int exp_handle_performance(void) {
    printf("[exp3] Testing open-handle vs per-op-open performance...\n");

    const int ITERATIONS = 500;
    uint8_t block[512];
    memset(block, 0x42, 512);

    // Test A: Keep handle open, seek+write
    FILE *f = fopen("/opfs/perftest_open.bin", "w+b");
    if (!f) {
        printf("[exp3] FAIL: fopen: %s\n", strerror(errno));
        return 1;
    }

    double t0 = emscripten_get_now();
    for (int i = 0; i < ITERATIONS; i++) {
        size_t offset = (size_t)(i % 100) * 512;
        fseek(f, offset, SEEK_SET);
        fwrite(block, 512, 1, f);
    }
    double t1 = emscripten_get_now();
    fclose(f);
    double open_handle_ms = t1 - t0;

    // Test B: Open+write+close per operation
    // First create the file
    f = fopen("/opfs/perftest_reopen.bin", "wb");
    if (!f) {
        printf("[exp3] FAIL: fopen reopen: %s\n", strerror(errno));
        return 1;
    }
    // Pre-allocate by writing some data
    for (int i = 0; i < 100; i++) {
        fwrite(block, 512, 1, f);
    }
    fclose(f);

    t0 = emscripten_get_now();
    for (int i = 0; i < ITERATIONS; i++) {
        f = fopen("/opfs/perftest_reopen.bin", "r+b");
        if (!f) {
            printf("[exp3] FAIL: reopen iteration %d\n", i);
            return 1;
        }
        size_t offset = (size_t)(i % 100) * 512;
        fseek(f, offset, SEEK_SET);
        fwrite(block, 512, 1, f);
        fclose(f);
    }
    t1 = emscripten_get_now();
    double reopen_ms = t1 - t0;

    printf("[exp3] open-handle: %.1f ms (%d writes)\n", open_handle_ms, ITERATIONS);
    printf("[exp3] reopen-each: %.1f ms (%d writes)\n", reopen_ms, ITERATIONS);
    printf("[exp3] ratio: %.1fx\n", reopen_ms / open_handle_ms);

    unlink("/opfs/perftest_open.bin");
    unlink("/opfs/perftest_reopen.bin");

    printf("[exp3] PASS\n");
    return 0;
}

// ============================================================================
// Experiment 4: Sparse file behavior
// ============================================================================
static int exp_sparse_file(void) {
    printf("[exp4] Testing sparse file behavior...\n");

    FILE *f = fopen("/opfs/sparse.bin", "w+b");
    if (!f) {
        printf("[exp4] FAIL: fopen: %s\n", strerror(errno));
        return 1;
    }

    // Seek to 1MB and write 1 byte
    fseek(f, 1024 * 1024, SEEK_SET);
    uint8_t byte = 0xFF;
    fwrite(&byte, 1, 1, f);
    fclose(f);

    // Check reported size
    struct stat st;
    if (stat("/opfs/sparse.bin", &st) != 0) {
        printf("[exp4] FAIL: stat: %s\n", strerror(errno));
        return 1;
    }
    printf("[exp4] logical size: %ld bytes\n", (long)st.st_size);
    printf("[exp4] st_blocks: %ld (512-byte units)\n", (long)st.st_blocks);

    // Read from the gap (should be zeros)
    f = fopen("/opfs/sparse.bin", "rb");
    if (!f) {
        printf("[exp4] FAIL: fopen read: %s\n", strerror(errno));
        return 1;
    }
    uint8_t buf[512];
    fread(buf, 512, 1, f);
    fclose(f);
    int all_zero = 1;
    for (int i = 0; i < 512; i++) {
        if (buf[i] != 0) {
            all_zero = 0;
            break;
        }
    }
    printf("[exp4] gap reads as zeros: %s\n", all_zero ? "yes" : "NO");

    unlink("/opfs/sparse.bin");
    printf("[exp4] PASS\n");
    return 0;
}

// ============================================================================
// Experiment 5: Persistence marker (write file, read back after reload)
// ============================================================================
static int exp_persistence_write(void) {
    printf("[exp5] Writing persistence marker...\n");

    if (mkdir("/opfs/persist_test", 0777) != 0 && errno != EEXIST) {
        printf("[exp5] FAIL: mkdir: %s\n", strerror(errno));
        return 1;
    }

    FILE *f = fopen("/opfs/persist_test/marker.txt", "w");
    if (!f) {
        printf("[exp5] FAIL: fopen: %s\n", strerror(errno));
        return 1;
    }
    fprintf(f, "PERSIST_OK_12345");
    fclose(f);

    printf("[exp5] PASS (marker written)\n");
    return 0;
}

// Read-only check (no cleanup)
static int exp_persistence_check(void) {
    printf("[exp5b] Checking persistence marker...\n");

    FILE *f = fopen("/opfs/persist_test/marker.txt", "r");
    if (!f) {
        printf("[exp5b] FAIL: marker file not found (errno=%d: %s)\n", errno, strerror(errno));
        return 1;
    }
    char buf[64] = {0};
    fgets(buf, sizeof(buf), f);
    fclose(f);

    if (strcmp(buf, "PERSIST_OK_12345") != 0) {
        printf("[exp5b] FAIL: marker content = '%s'\n", buf);
        return 1;
    }

    printf("[exp5b] PASS (persistence verified)\n");
    return 0;
}

// Cleanup after persistence test
static int exp_persistence_cleanup(void) {
    unlink("/opfs/persist_test/marker.txt");
    rmdir("/opfs/persist_test");
    printf("[exp5c] cleanup done\n");
    return 0;
}

// ============================================================================
// Experiment 6: main_loop with file I/O
// ============================================================================
static int g_tick_count = 0;
static int g_tick_io_ok = 1;

static void tick_with_io(void) {
    g_tick_count++;
    if (g_tick_count <= 3) {
        // Do file I/O during tick
        char path[64];
        snprintf(path, sizeof(path), "/opfs/tick_%d.tmp", g_tick_count);
        FILE *f = fopen(path, "w");
        if (!f) {
            printf("[exp6] FAIL: fopen in tick %d: %s\n", g_tick_count, strerror(errno));
            g_tick_io_ok = 0;
        } else {
            fprintf(f, "tick%d", g_tick_count);
            fclose(f);
        }
    }
    if (g_tick_count >= 5) {
        // Verify files written during ticks
        for (int i = 1; i <= 3; i++) {
            char path[64];
            snprintf(path, sizeof(path), "/opfs/tick_%d.tmp", i);
            FILE *f = fopen(path, "r");
            if (!f) {
                printf("[exp6] FAIL: tick file %d not found\n", i);
                g_tick_io_ok = 0;
                continue;
            }
            char buf[16] = {0};
            fgets(buf, sizeof(buf), f);
            fclose(f);
            char expected[16];
            snprintf(expected, sizeof(expected), "tick%d", i);
            if (strcmp(buf, expected) != 0) {
                printf("[exp6] FAIL: tick %d content = '%s'\n", i, buf);
                g_tick_io_ok = 0;
            }
            unlink(path);
        }
        if (g_tick_io_ok) {
            printf("[exp6] PASS\n");
        }
        emscripten_cancel_main_loop();
    }
}

static int exp_main_loop_io(void) {
    printf("[exp6] Testing emscripten_set_main_loop with file I/O...\n");
    g_tick_count = 0;
    g_tick_io_ok = 1;
    // This will run asynchronously — the result is printed from the tick function
    emscripten_set_main_loop(tick_with_io, 0, 0);
    return 0; // Result reported asynchronously
}

// ============================================================================
// Dispatch
// ============================================================================
EMSCRIPTEN_KEEPALIVE
int run_experiment(const char *name) {
    if (strcmp(name, "filesystem_setup") == 0)
        return exp_filesystem_setup();
    if (strcmp(name, "seek_write") == 0)
        return exp_seek_write_granularity();
    if (strcmp(name, "handle_performance") == 0)
        return exp_handle_performance();
    if (strcmp(name, "sparse_file") == 0)
        return exp_sparse_file();
    if (strcmp(name, "persistence_write") == 0)
        return exp_persistence_write();
    if (strcmp(name, "persistence_check") == 0)
        return exp_persistence_check();
    if (strcmp(name, "persistence_cleanup") == 0)
        return exp_persistence_cleanup();
    if (strcmp(name, "main_loop_io") == 0)
        return exp_main_loop_io();
    printf("Unknown experiment: %s\n", name);
    return -1;
}

// ============================================================================
// Filesystem setup: OPFS mounted at /opfs (matches Emscripten's own test pattern)
// ============================================================================

int main(void) {
    backend_t opfs = wasmfs_create_opfs_backend();
    int rc = wasmfs_create_directory("/opfs", 0777, opfs);
    if (rc != 0) {
        printf("FATAL: failed to create /opfs (rc=%d, errno=%d: %s)\n", rc, errno, strerror(errno));
        return 1;
    }
    printf("Filesystem ready: /opfs = OPFS\n");
    return 0;
}
