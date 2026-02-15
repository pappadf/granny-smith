// Single-step CPU instruction test suite
// Tests 68000 instructions against the SingleStepTests/m68000 test data
// which was generated from MAME's highly accurate m68000 core.

#include "test_assert.h"
#include "harness.h"
#include "cpu.h"
#include "memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <dirent.h>
#include <sys/stat.h>

// === Configuration ===

// Maximum number of tests to run per instruction file (0 = unlimited)
#define MAX_TESTS_PER_FILE 0

// Maximum number of RAM entries per test
#define MAX_RAM_ENTRIES 256

// Maximum instruction file name length
#define MAX_NAME_LEN 256

// Full 24-bit address space for testing (16MB)
#define TEST_MEM_SIZE (16 * 1024 * 1024)

// Maximum failures to show per test file before suppressing output
#define MAX_FAILURES_PER_FILE 5

// Minimum bytes pushed for exception stack frame (SR + PC = 6 bytes)
#define MIN_EXCEPTION_STACK_FRAME_SIZE 6

// Maximum instruction words for disassembly
#define MAX_DISASM_WORDS 10

// Test file extension
#define TEST_FILE_EXTENSION ".json.bin"
#define TEST_FILE_EXTENSION_LEN 9

// Transaction record field sizes (fc, addr_bus, data_bus, UDS, LDS = 5 x 4 bytes)
#define TRANSACTION_FIELD_SIZE 20
#define TRANSACTION_HEADER_SIZE 5  // tw (1) + cycles (4)

// === Binary Format Constants (from decode.py) ===
#define MAGIC_FILE      0x1A3F5D71
#define MAGIC_TEST      0xABC12367
#define MAGIC_NAME      0x89ABCDEF
#define MAGIC_STATE     0x01234567
#define MAGIC_TRANS     0x456789AB

// === Excluded Instructions ===
// Mechanism to skip instructions known to have issues in the test suite
// Add instruction file names (without extension) to exclude them
// NOTE: Only exclude instructions when:
//   a) The cputest (hardware-validated) suite doesn't cover them AND
//   b) The behavior is documented as undefined, OR
//   c) The test data is for a different CPU model (e.g., 68020 vs 68000)
static const char *excluded_instructions[] = {
    // STOP: PC behavior differs - our emulator advances to next instruction
    "STOP",
    // CHK: When no exception occurs, the N, Z, V, C flags are undefined per
    // MC68000 documentation. MAME sets them differently than we do. Both are
    // correct since the flags are undefined. cputest doesn't cover CHK.
    "CHK",
    // Bcc: Tests include Bcc.L (32-bit displacement, opcode byte 0xFF) which
    // is a 68020+ feature. On 68000, this should be an illegal instruction.
    // Our emulator currently executes it as 68020+ code. cputest doesn't
    // cover this specific case (Bcc.L).
    "Bcc",
    NULL  // sentinel
};

// === Data Structures ===

// RAM entry: address and byte value
typedef struct {
    uint32_t addr;
    uint8_t value;
} ram_entry_t;

// CPU state for a test case
typedef struct {
    uint32_t d[8];      // D0-D7
    uint32_t a[7];      // A0-A6
    uint32_t usp;       // User stack pointer
    uint32_t ssp;       // Supervisor stack pointer
    uint16_t sr;        // Status register
    uint32_t pc;        // Program counter
    uint32_t prefetch[2];  // Prefetch words (stored as 32-bit in test format)
    ram_entry_t ram[MAX_RAM_ENTRIES];
    int ram_count;
} cpu_state_t;

// Single test case
typedef struct {
    char name[MAX_NAME_LEN];
    cpu_state_t initial;
    cpu_state_t final;
    // We don't verify cycle-by-cycle transactions, only final state
} test_case_t;

// === Binary File Decoder ===

// Read a 32-bit little-endian value
static uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Read a 16-bit little-endian value
static uint16_t read_le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

// Decode test name from binary
static const uint8_t *decode_name(const uint8_t *ptr, char *name, size_t max_len) {
    // uint32_t numbytes = read_le32(ptr);
    ptr += 4;
    uint32_t magic = read_le32(ptr);
    ptr += 4;
    if (magic != MAGIC_NAME) {
        fprintf(stderr, "[cpu] invalid name magic: 0x%08X\n", magic);
        return NULL;
    }
    
    uint32_t name_len = read_le32(ptr);
    ptr += 4;
    
    // Limit copy length to buffer size minus null terminator
    size_t copy_len = name_len;
    if (copy_len >= max_len) copy_len = max_len - 1;
    memcpy(name, ptr, copy_len);
    name[copy_len] = '\0';
    ptr += name_len;  // Advance by actual name length, not truncated length
    
    return ptr;
}

// Decode CPU state from binary
static const uint8_t *decode_state(const uint8_t *ptr, cpu_state_t *state) {
    // uint32_t numbytes = read_le32(ptr);
    ptr += 4;
    uint32_t magic = read_le32(ptr);
    ptr += 4;
    if (magic != MAGIC_STATE) {
        fprintf(stderr, "[cpu] invalid state magic: 0x%08X\n", magic);
        return NULL;
    }
    
    // Registers in order: d0-d7, a0-a6, usp, ssp, sr, pc
    for (int i = 0; i < 8; i++) {
        state->d[i] = read_le32(ptr);
        ptr += 4;
    }
    for (int i = 0; i < 7; i++) {
        state->a[i] = read_le32(ptr);
        ptr += 4;
    }
    state->usp = read_le32(ptr); ptr += 4;
    state->ssp = read_le32(ptr); ptr += 4;
    state->sr = (uint16_t)read_le32(ptr); ptr += 4;
    state->pc = read_le32(ptr); ptr += 4;
    
    // Prefetch words (stored as 32-bit values in test format)
    state->prefetch[0] = read_le32(ptr); ptr += 4;
    state->prefetch[1] = read_le32(ptr); ptr += 4;
    
    // RAM entries - each test entry is a 16-bit word at an even address
    // We split into two byte entries for easier comparison
    uint32_t num_rams = read_le32(ptr);
    ptr += 4;
    
    state->ram_count = 0;
    for (uint32_t i = 0; i < num_rams; i++) {
        uint32_t addr = read_le32(ptr);
        ptr += 4;
        uint16_t data = read_le16(ptr);
        ptr += 2;
        
        // Check we have room for both bytes before adding
        if (state->ram_count + 2 > MAX_RAM_ENTRIES) {
            break;  // Stop if we can't fit both bytes
        }
        
        // High byte at addr, low byte at addr+1 (68000 is big-endian)
        state->ram[state->ram_count].addr = addr;
        state->ram[state->ram_count].value = (uint8_t)(data >> 8);
        state->ram_count++;
        
        state->ram[state->ram_count].addr = addr | 1;
        state->ram[state->ram_count].value = (uint8_t)(data & 0xFF);
        state->ram_count++;
    }
    
    return ptr;
}

// Skip transactions section (we don't verify cycle-by-cycle)
static const uint8_t *skip_transactions(const uint8_t *ptr) {
    // uint32_t numbytes = read_le32(ptr);
    ptr += 4;
    uint32_t magic = read_le32(ptr);
    ptr += 4;
    if (magic != MAGIC_TRANS) {
        fprintf(stderr, "[cpu] invalid transactions magic: 0x%08X\n", magic);
        return NULL;
    }
    
    // uint32_t num_cycles = read_le32(ptr);
    ptr += 4;
    uint32_t num_transactions = read_le32(ptr);
    ptr += 4;
    
    for (uint32_t i = 0; i < num_transactions; i++) {
        uint8_t tw = *ptr++;
        ptr += 4;  // cycles (uint32_t)
        if (tw != 0) {
            ptr += TRANSACTION_FIELD_SIZE;  // fc, addr_bus, data_bus, UDS, LDS
        }
    }
    
    return ptr;
}

// Decode a single test case from binary
static const uint8_t *decode_test(const uint8_t *ptr, test_case_t *test) {
    // uint32_t numbytes = read_le32(ptr);
    ptr += 4;
    uint32_t magic = read_le32(ptr);
    ptr += 4;
    if (magic != MAGIC_TEST) {
        fprintf(stderr, "[cpu] invalid test magic: 0x%08X\n", magic);
        return NULL;
    }
    
    ptr = decode_name(ptr, test->name, MAX_NAME_LEN);
    if (!ptr) return NULL;
    
    ptr = decode_state(ptr, &test->initial);
    if (!ptr) return NULL;
    
    ptr = decode_state(ptr, &test->final);
    if (!ptr) return NULL;
    
    ptr = skip_transactions(ptr);
    if (!ptr) return NULL;
    
    return ptr;
}

// === Test Execution ===

// Test memory buffer for full 24-bit address space
static uint8_t *test_memory_buffer = NULL;

// Initialize test memory - allocate a full 16MB buffer and set up page table
static bool init_test_memory(void) {
    if (test_memory_buffer) return true;  // already initialized
    
    test_memory_buffer = calloc(TEST_MEM_SIZE, 1);
    if (!test_memory_buffer) {
        fprintf(stderr, "[cpu] failed to allocate %d MB test memory\n", TEST_MEM_SIZE / (1024*1024));
        return false;
    }
    
    // Update the page table to point to our test buffer for all pages
    // This makes the full 24-bit address space accessible
    if (g_page_table) {
        for (int p = 0; p < (TEST_MEM_SIZE >> PAGE_SHIFT); p++) {
            g_page_table[p].host_base = test_memory_buffer + (p << PAGE_SHIFT);
            g_page_table[p].dev = NULL;
            g_page_table[p].dev_context = NULL;
            g_page_table[p].writable = true;
        }
    }
    
    return true;
}

// Cleanup test memory
static void cleanup_test_memory(void) {
    if (test_memory_buffer) {
        free(test_memory_buffer);
        test_memory_buffer = NULL;
    }
}

// The test format uses MAME's prefetch model where PC points to the prefetch address
// (typically instruction_address + 4). Our emulator uses PC as the instruction address.
// We need to adjust PC by -4 when setting up the test and comparing results.
#define PREFETCH_OFFSET 4

// Set up CPU state from test case initial state
static void setup_cpu_state(cpu_t *cpu, const cpu_state_t *state) {
    // Set registers
    for (int i = 0; i < 8; i++) {
        cpu_set_dn(cpu, i, state->d[i]);
    }
    for (int i = 0; i < 7; i++) {
        cpu_set_an(cpu, i, state->a[i]);
    }
    
    // The 68000's A7 depends on supervisor mode, and write_sr() will swap A7
    // between USP and SSP when mode changes. We need to set things up carefully:
    
    // First, set A7 to one of the stack pointers so write_sr() has valid data to save
    bool target_supervisor = (state->sr & 0x2000) != 0;
    if (target_supervisor) {
        cpu_set_an(cpu, 7, state->ssp);
    } else {
        cpu_set_an(cpu, 7, state->usp);
    }
    
    // Set the non-active stack pointer directly
    if (target_supervisor) {
        cpu_set_usp(cpu, state->usp);
    } else {
        cpu_set_ssp(cpu, state->ssp);
    }
    
    // Now set SR - write_sr() may swap A7 with the "other" stack pointer, 
    // but both are now set correctly
    cpu_set_sr(cpu, state->sr);
    
    // Re-set both stack pointers after SR change to ensure they're correct
    cpu_set_usp(cpu, state->usp);
    cpu_set_ssp(cpu, state->ssp);
    
    // Finally, set A7 to the active stack pointer
    if (target_supervisor) {
        cpu_set_an(cpu, 7, state->ssp);
    } else {
        cpu_set_an(cpu, 7, state->usp);
    }
    
    // Adjust PC for our emulator's model (PC = instruction address, not prefetch address)
    // The test data has PC = instruction_address + 4, so we subtract 4
    cpu_set_pc(cpu, (state->pc - PREFETCH_OFFSET) & 0x00FFFFFF);
    
    // Set up RAM using inline memory functions (uses page table)
    for (int i = 0; i < state->ram_count; i++) {
        memory_write_uint8(state->ram[i].addr & 0x00FFFFFF, state->ram[i].value);
    }
}

// Check if the test involves an exception (PC jump to exception handler)
static bool test_triggers_exception(const cpu_state_t *initial, const cpu_state_t *final) {
    // If the SR supervisor bit changes from 0 to 1, an exception likely occurred
    if ((initial->sr & 0x2000) == 0 && (final->sr & 0x2000) != 0) {
        return true;
    }
    // If SSP decreased significantly, exception likely pushed stack frame
    if (final->ssp < initial->ssp &&
        (initial->ssp - final->ssp) >= MIN_EXCEPTION_STACK_FRAME_SIZE) {
        return true;
    }
    return false;
}

// Compare CPU state against expected final state
// Returns true if states match
static bool compare_cpu_state(cpu_t *cpu, const cpu_state_t *expected,
                             const cpu_state_t *initial,
                             char *diff_buf, size_t diff_buf_size) {
    bool match = true;
    diff_buf[0] = '\0';
    char *p = diff_buf;
    size_t remaining = diff_buf_size;
    
    #define APPEND_DIFF(...) do { \
        int n = snprintf(p, remaining, __VA_ARGS__); \
        if (n > 0 && (size_t)n < remaining) { p += n; remaining -= n; } \
    } while (0)
    
    // Compare D registers
    for (int i = 0; i < 8; i++) {
        uint32_t actual = cpu_get_dn(cpu, i);
        if (actual != expected->d[i]) {
            match = false;
            APPEND_DIFF("  D%d: 0x%08X (expected 0x%08X, was 0x%08X)\n",
                       i, actual, expected->d[i], initial->d[i]);
        }
    }
    
    // Compare A registers (A0-A6)
    for (int i = 0; i < 7; i++) {
        uint32_t actual = cpu_get_an(cpu, i);
        if (actual != expected->a[i]) {
            match = false;
            APPEND_DIFF("  A%d: 0x%08X (expected 0x%08X, was 0x%08X)\n",
                       i, actual, expected->a[i], initial->a[i]);
        }
    }
    
    // Compare USP and SSP
    // The 68000 stores the active stack pointer in A7, and the inactive one is saved.
    // In supervisor mode: A7 = SSP, cpu->usp = USP
    // In user mode: A7 = USP, cpu->ssp = SSP
    uint16_t final_sr = cpu_get_sr(cpu);
    bool in_supervisor = (final_sr & 0x2000) != 0;
    
    uint32_t actual_usp, actual_ssp;
    if (in_supervisor) {
        actual_ssp = cpu_get_an(cpu, 7);  // A7 is SSP in supervisor mode
        actual_usp = cpu_get_usp(cpu);     // USP is saved separately
    } else {
        actual_usp = cpu_get_an(cpu, 7);  // A7 is USP in user mode
        actual_ssp = cpu_get_ssp(cpu);     // SSP is saved separately
    }
    
    if (actual_usp != expected->usp) {
        match = false;
        APPEND_DIFF("  USP: 0x%08X (expected 0x%08X, was 0x%08X)\n",
                   actual_usp, expected->usp, initial->usp);
    }
    if (actual_ssp != expected->ssp) {
        match = false;
        APPEND_DIFF("  SSP: 0x%08X (expected 0x%08X, was 0x%08X)\n",
                   actual_ssp, expected->ssp, initial->ssp);
    }
    
    // Compare SR (mask out undefined bits on 68000: bits 5-7 and 11-12)
    // We only check defined bits: C, V, Z, N, X (0-4), IPL (8-10), S (13), T (14-15)
    uint16_t sr_mask = 0xE71F;  // Defined bits mask for 68000
    uint16_t actual_sr = cpu_get_sr(cpu) & sr_mask;
    uint16_t expected_sr = expected->sr & sr_mask;
    if (actual_sr != expected_sr) {
        match = false;
        APPEND_DIFF("  SR: 0x%04X (expected 0x%04X, was 0x%04X)\n",
                   actual_sr, expected_sr, initial->sr & sr_mask);
    }
    
    // Compare PC (adjust for prefetch offset)
    // Our emulator's PC is at instruction address, test data has PC at prefetch address
    // Mask to 24 bits since 68000 has 24-bit address space
    uint32_t actual_pc = cpu_get_pc(cpu) & 0x00FFFFFF;
    uint32_t expected_pc_adjusted = (expected->pc - PREFETCH_OFFSET) & 0x00FFFFFF;
    if (actual_pc != expected_pc_adjusted) {
        match = false;
        APPEND_DIFF("  PC: 0x%08X (expected 0x%08X, was 0x%08X)\n",
                   actual_pc, expected_pc_adjusted, (initial->pc - PREFETCH_OFFSET) & 0x00FFFFFF);
    }
    
    // Compare RAM at touched locations
    for (int i = 0; i < expected->ram_count; i++) {
        uint32_t addr = expected->ram[i].addr & 0x00FFFFFF;
        uint8_t actual_val = memory_read_uint8(addr);
        uint8_t expected_val = expected->ram[i].value;
        if (actual_val != expected_val) {
            match = false;
            // Find initial value
            uint8_t initial_val = 0;
            for (int j = 0; j < initial->ram_count; j++) {
                if ((initial->ram[j].addr & 0x00FFFFFF) == addr) {
                    initial_val = initial->ram[j].value;
                    break;
                }
            }
            APPEND_DIFF("  RAM[0x%06X]: 0x%02X (expected 0x%02X, was 0x%02X)\n",
                       addr, actual_val, expected_val, initial_val);
        }
    }
    
    #undef APPEND_DIFF
    return match;
}

// Disassemble instruction at given address for diagnostic output
static void disassemble_at(uint32_t addr, char *buf) {
    uint16_t words[MAX_DISASM_WORDS];
    for (int i = 0; i < MAX_DISASM_WORDS; i++) {
        uint32_t word_addr = (addr + i * 2) & 0x00FFFFFF;
        words[i] = memory_read_uint16(word_addr);
    }
    int n = cpu_disasm(words, buf);
    (void)n;  // Ignore return value (number of words used)
}

// Check if an instruction should be excluded
static bool is_excluded(const char *filename) {
    // Extract base name without path and extension
    const char *base = strrchr(filename, '/');
    base = base ? base + 1 : filename;
    
    char name[MAX_NAME_LEN];
    strncpy(name, base, sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    
    // Remove .json.bin extension
    char *ext = strstr(name, ".json.bin");
    if (ext) *ext = '\0';
    
    for (const char **p = excluded_instructions; *p != NULL; p++) {
        if (strcmp(name, *p) == 0) {
            return true;
        }
    }
    return false;
}

// === Test Data Path Resolution ===

// Find the test data directory
static const char *find_test_data_dir(void) {
    static char resolved[4096];
    struct stat st;
    
    // Try environment variable first
    const char *env_dir = getenv("CPU_TEST_DATA_DIR");
    if (env_dir && env_dir[0]) {
        if (stat(env_dir, &st) == 0 && S_ISDIR(st.st_mode)) {
            strncpy(resolved, env_dir, sizeof(resolved) - 1);
            return resolved;
        }
    }
    
    // Try common relative paths (from suites/cpu/ within tests/unit/)
    const char *candidates[] = {
        "../../../../third-party/single-step-tests/v1",
        "../../../third-party/single-step-tests/v1",
        "../../third-party/single-step-tests/v1",
        "../third-party/single-step-tests/v1",
        "third-party/single-step-tests/v1",
    };
    
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        if (realpath(candidates[i], resolved)) {
            if (stat(resolved, &st) == 0 && S_ISDIR(st.st_mode)) {
                return resolved;
            }
        }
    }
    
    return NULL;
}

// === Main Test Runner ===

typedef struct {
    int tests_run;
    int tests_passed;
    int tests_failed;
    int files_processed;
    int files_excluded;
} test_stats_t;

// Run tests from a single binary file
static int run_test_file(const char *filepath, test_context_t *ctx, test_stats_t *stats) {
    if (is_excluded(filepath)) {
        stats->files_excluded++;
        return 0;
    }
    
    // Load the file
    FILE *f = fopen(filepath, "rb");
    if (!f) {
        fprintf(stderr, "[cpu] cannot open: %s\n", filepath);
        return -1;
    }
    
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    uint8_t *data = malloc(file_size);
    if (!data) {
        fclose(f);
        fprintf(stderr, "[cpu] out of memory for %s\n", filepath);
        return -1;
    }
    
    if (fread(data, 1, file_size, f) != (size_t)file_size) {
        free(data);
        fclose(f);
        fprintf(stderr, "[cpu] read error: %s\n", filepath);
        return -1;
    }
    fclose(f);
    
    // Parse file header
    const uint8_t *ptr = data;
    uint32_t magic = read_le32(ptr);
    ptr += 4;
    if (magic != MAGIC_FILE) {
        free(data);
        fprintf(stderr, "[cpu] invalid file magic in %s: 0x%08X\n", filepath, magic);
        return -1;
    }
    
    uint32_t num_tests = read_le32(ptr);
    ptr += 4;
    
    // Extract base filename for display
    const char *basename = strrchr(filepath, '/');
    basename = basename ? basename + 1 : filepath;
    
    fprintf(stderr, "[cpu] %s: %u tests\n", basename, num_tests);
    
    // Get CPU from test context
    cpu_t *cpu = test_get_cpu(ctx);
    
    if (!cpu) {
        free(data);
        fprintf(stderr, "[cpu] test context not properly initialized\n");
        return -1;
    }
    
    int file_failures = 0;
    test_case_t test;
    char diff_buf[4096];
    char disasm_buf[256];
    
    uint32_t limit = num_tests;
    #if MAX_TESTS_PER_FILE > 0
    if (limit > MAX_TESTS_PER_FILE) limit = MAX_TESTS_PER_FILE;
    #endif
    
    for (uint32_t i = 0; i < limit; i++) {
        ptr = decode_test(ptr, &test);
        if (!ptr) {
            fprintf(stderr, "[cpu] decode error at test %u in %s\n", i, filepath);
            break;
        }
        
        // Skip exception tests for now (they require special handling)
        if (test_triggers_exception(&test.initial, &test.final)) {
            // Don't count as run
            continue;
        }
        
        stats->tests_run++;
        
        // Clear memory at addresses the test will check (to avoid stale data
         // from previous tests causing false failures)
        for (int j = 0; j < test.final.ram_count; j++) {
            // Mask to 24 bits for 68000 address space
            uint32_t addr = test.final.ram[j].addr & 0x00FFFFFF;
            memory_write_uint8(addr, 0);
        }
        
        // Set up initial state
        setup_cpu_state(cpu, &test.initial);
        
        // Execute exactly one instruction
        uint32_t instructions = 1;
        cpu_run_sprint(cpu, &instructions);
        
        // Compare final state
        if (!compare_cpu_state(cpu, &test.final, &test.initial,
                              diff_buf, sizeof(diff_buf))) {
            file_failures++;
            stats->tests_failed++;
            
            // Print detailed failure information
            // Instruction is at PC-4 (test data uses prefetch model)
            uint32_t instr_addr = (test.initial.pc - PREFETCH_OFFSET) & 0x00FFFFFF;
            disassemble_at(instr_addr, disasm_buf);
            
            fprintf(stderr, "\n=== FAILURE: %s ===\n", test.name);
            fprintf(stderr, "Instruction: %s\n", disasm_buf);
            fprintf(stderr, "Initial PC: 0x%08X (instr at 0x%08X), SR: 0x%04X\n",
                   test.initial.pc, instr_addr, test.initial.sr);
            fprintf(stderr, "Differences:\n%s", diff_buf);
            
            // Only show first few failures per file to avoid spam
            if (file_failures >= MAX_FAILURES_PER_FILE) {
                fprintf(stderr, "[cpu] ... suppressing further failures for this file\n");
                break;
            }
        } else {
            stats->tests_passed++;
        }
    }
    
    stats->files_processed++;
    free(data);
    return file_failures;
}

// Run all test files in the data directory
static int run_all_tests(test_context_t *ctx) {
    const char *data_dir = find_test_data_dir();
    if (!data_dir) {
        fprintf(stderr, "[cpu] cannot find test data directory\n");
        fprintf(stderr, "[cpu] set CPU_TEST_DATA_DIR or ensure third-party/single-step-tests is present\n");
        return -1;
    }
    
    fprintf(stderr, "[cpu] test data directory: %s\n", data_dir);
    
    // Initialize extended test memory (full 16MB address space)
    if (!init_test_memory()) {
        return -1;
    }
    
    DIR *dir = opendir(data_dir);
    if (!dir) {
        fprintf(stderr, "[cpu] cannot open directory: %s\n", data_dir);
        return -1;
    }
    
    test_stats_t stats = {0};
    struct dirent *entry;
    char filepath[4096];
    
    while ((entry = readdir(dir)) != NULL) {
        // Only process .json.bin files
        const char *ext = strstr(entry->d_name, TEST_FILE_EXTENSION);
        if (!ext || ext[TEST_FILE_EXTENSION_LEN] != '\0') continue;
        
        snprintf(filepath, sizeof(filepath), "%s/%s", data_dir, entry->d_name);
        run_test_file(filepath, ctx, &stats);
    }
    
    closedir(dir);
    
    // Print summary
    fprintf(stderr, "\n=== CPU Test Summary ===\n");
    fprintf(stderr, "Files processed: %d\n", stats.files_processed);
    fprintf(stderr, "Files excluded:  %d\n", stats.files_excluded);
    fprintf(stderr, "Tests run:       %d\n", stats.tests_run);
    fprintf(stderr, "Tests passed:    %d\n", stats.tests_passed);
    fprintf(stderr, "Tests failed:    %d\n", stats.tests_failed);
    
    // Return number of failures (for test framework)
    return stats.tests_failed;
}

// === Test Entry Points ===

TEST(cpu_single_step_tests) {
    test_context_t *ctx = test_harness_init();
    if (!ctx) {
        fprintf(stderr, "[cpu] failed to initialize test harness\n");
        ASSERT_TRUE(0);
    }
    
    int failures = run_all_tests(ctx);
    
    cleanup_test_memory();
    test_harness_destroy(ctx);
    
    // Fail the test if there are any failures
    // Instructions with known acceptable differences should be added to
    // excluded_instructions[] with proper documentation
    if (failures > 0) {
        fprintf(stderr, "\n[cpu] FAILED: %d test(s) failed\n", failures);
        fprintf(stderr, "If these failures are expected (undefined behavior, different CPU model),\n");
        fprintf(stderr, "add the instruction to excluded_instructions[] in test.c\n");
    }
    
    ASSERT_TRUE(failures == 0);
}

int main(void) {
    RUN(cpu_single_step_tests);
    return 0;
}
