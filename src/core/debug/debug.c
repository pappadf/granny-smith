// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// debug.c
// Debugging, breakpoints, logpoints, and trace functionality.

// ============================================================================
// Includes
// ============================================================================

#include "debug.h"

#include "addr_format.h"
#include "cmd_symbol.h"
#include "cmd_types.h"
#include "common.h"
#include "cpu.h"
#include "cpu_internal.h"
#include "debug_mac.h"
#include "fpu.h"
#include "log.h"
#include "memory.h"
#include "mmu.h"
#include "scheduler.h"
#include "shell.h"
#include "system.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Type Definitions
// ============================================================================

struct breakpoint {

    uint32_t addr;
    addr_space_t space; // logical or physical address space

    breakpoint_t *next;
};

// Logpoint structure - like breakpoint but doesn't stop execution
struct logpoint {

    uint32_t addr; // start address of range
    uint32_t end_addr; // end address of range (inclusive), same as addr for single address

    // Log category and level for this logpoint
    log_category_t *category;
    int level;

    // Optional message to display when hit
    char *message;

    // Hit counter for this logpoint
    uint32_t hit_count;

    logpoint_t *next;
};

// ============================================================================
// Static Helpers
// ============================================================================

static uint16_t cpu_get_uint16(uint32_t addr) {
    if (!system_memory())
        return 0;
    return memory_read_uint16(addr);
}

// ============================================================================
// Operations
// ============================================================================

breakpoint_t *set_breakpoint(debug_t *debug, uint32_t addr, addr_space_t space) {

    breakpoint_t *bp = malloc(sizeof(breakpoint_t));

    if (bp == NULL)
        return NULL;

    bp->addr = addr;
    bp->space = space;

    // add bp to a linked list
    bp->next = debug->breakpoints;
    debug->breakpoints = bp;

    debug->active = true;

    return bp;
}

// Set a logpoint at the specified address range (end_addr == addr for single address)
logpoint_t *set_logpoint(debug_t *debug, uint32_t addr, uint32_t end_addr, log_category_t *category, int level) {

    logpoint_t *lp = malloc(sizeof(logpoint_t));

    if (lp == NULL)
        return NULL;

    lp->addr = addr;
    lp->end_addr = end_addr;
    lp->category = category;
    lp->level = level;
    lp->hit_count = 0;
    lp->message = NULL;

    // add lp to a linked list
    lp->next = debug->logpoints;
    debug->logpoints = lp;

    debug->active = true;

    return lp;
}

extern int cpu_disasm(uint16_t *instr, char *buf);
extern int disasm_68000(uint16_t *instr, char *buf);

// Forward declarations for trace functions
static void trace_add_pc_entry(debug_t *debug, uint32_t pc);

// Forward declarations for logpoint management (IMP-604)
static void list_logpoints(debug_t *debug);
static int delete_logpoint(debug_t *debug, uint32_t addr);
static void delete_all_logpoints(debug_t *debug);

static int disasm(uint16_t *instr, char *mnemonic, char *operands) {
    char buf[100];
    int i, n;

    n = cpu_disasm(instr, buf);

    if (strlen(buf) == 0) {
        sprintf(mnemonic, "ILLEGAL");
        sprintf(operands, "");
    } else {
        for (i = 0; buf[i] != '\0' && buf[i] != '\t'; i++)
            mnemonic[i] = buf[i];
        mnemonic[i] = '\0';
        if (buf[i] == '\t')
            sprintf(operands, "%s", buf + i + 1);
        else
            operands[0] = '\0';
    }

    return n;
}

// Disassemble instruction at addr, write to buf, return instruction length in words
int debugger_disasm(char *buf, uint32_t addr) {
    uint16_t words[16];
    char mnemonic[32], operands[32];

    int i;
    for (i = 0; i < 16; i++)
        words[i] = cpu_get_uint16(addr + i * 2);

    int n = disasm(words, mnemonic, operands);

    // Format with address prefix
    char addr_str[40];
    format_address_pair(addr_str, sizeof(addr_str), addr);
    sprintf(buf, "%s  %04x  %-10s%-12s", addr_str, (int)words[0], mnemonic, operands);

    return n;
}

// Disassemble instruction at current PC, write to buf
void debugger_disasm_pc(char *buf) {
    cpu_t *cpu = system_cpu();
    if (!cpu) {
        buf[0] = '\0';
        return;
    }
    debugger_disasm(buf, cpu_get_pc(cpu));
}

static void cmd_d(uint32_t n) {
    cpu_t *cpu = system_cpu();
    if (!cpu)
        return;
    uint32_t pc = cpu_get_pc(cpu);

    debug_t *debug = system_debug();

    for (unsigned int i = 0; i < n; i++) {
        char buf[100];
        pc += 2 * debugger_disasm(buf, pc);
        printf("%s\n", buf);
    }
}

// Check if execution should break and trace current instruction
int debug_break_and_trace(void) {
    debug_t *debug = system_debug();
    cpu_t *cpu = system_cpu();
    if (!debug || !cpu)
        return false;
    bool stop = false;
    uint32_t current_pc = cpu_get_pc(cpu);

    // If we have a last_breakpoint_pc set, this means we need to skip checking
    // for breakpoints at that specific PC address one time (to allow resuming execution)
    if (debug->last_breakpoint_pc != 0 && current_pc == debug->last_breakpoint_pc) {
        // Clear the flag after skipping once
        debug->last_breakpoint_pc = 0;
    } else {
        // Check for breakpoints at current PC
        breakpoint_t *bp = debug->breakpoints;
        while (bp != NULL) {
            bool hit = false;
            if (bp->space == ADDR_LOGICAL) {
                // Logical breakpoint: compare directly with PC
                hit = (bp->addr == current_pc);
            } else {
                // Physical breakpoint: translate PC to physical and compare
                bool is_identity, valid;
                uint32_t phys_pc = debug_translate_address(current_pc, &is_identity, NULL, &valid);
                hit = valid && (bp->addr == phys_pc);
            }
            if (hit) {
                if (bp->space == ADDR_PHYSICAL) {
                    printf("breakpoint hit at P:$%08X (PC=$%08X)\n", bp->addr, current_pc);
                } else {
                    printf("breakpoint hit at $%08X\n", bp->addr);
                }
                // Remember this PC to skip it next time we check
                debug->last_breakpoint_pc = current_pc;
                // Auto-delete temporary breakpoints (IMP-601)
                debug_check_tbreak(debug, current_pc);
                stop = true;
                break;
            }
            bp = bp->next;
        }
    }

    // Check for logpoints at current PC (these don't stop execution)
    logpoint_t *lp = debug->logpoints;
    while (lp != NULL) {
        // Check if current_pc is within the logpoint's address range
        if (current_pc >= lp->addr && current_pc <= lp->end_addr) {
            // Increment hit counter
            lp->hit_count++;
            // Log using the log framework with optional message
            if (lp->message) {
                LOG_WITH(lp->category, lp->level, "logpoint $%08X: %s", current_pc, lp->message);
            } else {
                LOG_WITH(lp->category, lp->level, "logpoint hit at $%08X (hit count: %u)", current_pc, lp->hit_count);
            }
        }
        lp = lp->next;
    }

    if (debug->trace_buffer) {
        debug->trace_buffer[debug->trace_head] = cpu_get_pc(cpu);
        debug->trace_head = (debug->trace_head + 1) % debug->trace_buffer_size;
        if (debug->trace_head == debug->trace_tail)
            debug->trace_tail = (debug->trace_tail + 1) % debug->trace_buffer_size;
    }

    // Record PC in new trace entries buffer
    if (debug->trace_entries) {
        trace_add_pc_entry(debug, cpu_get_pc(cpu));
    }

    return stop;
}

// Delete a breakpoint at the specified address and space
// Returns true if breakpoint was found and deleted, false otherwise
bool delete_breakpoint(debug_t *debug, uint32_t addr, addr_space_t space) {
    breakpoint_t *prev = NULL;
    breakpoint_t *bp = debug->breakpoints;

    while (bp != NULL) {
        if (bp->addr == addr && bp->space == space) {
            // Found the breakpoint, remove it from the list
            if (prev == NULL) {
                debug->breakpoints = bp->next;
            } else {
                prev->next = bp->next;
            }
            free(bp);
            return true;
        }
        prev = bp;
        bp = bp->next;
    }
    return false;
}

// Delete all breakpoints
// Returns the number of breakpoints deleted
static int delete_all_breakpoints(debug_t *debug) {
    int count = 0;
    breakpoint_t *bp = debug->breakpoints;

    while (bp != NULL) {
        breakpoint_t *next = bp->next;
        free(bp);
        bp = next;
        count++;
    }
    debug->breakpoints = NULL;
    return count;
}

// List all breakpoints
static void list_breakpoints(debug_t *debug) {
    breakpoint_t *bp = debug->breakpoints;
    int count = 0;

    if (bp == NULL) {
        printf("No breakpoints set.\n");
        return;
    }

    printf("Breakpoints:\n");
    while (bp != NULL) {
        if (bp->space == ADDR_PHYSICAL) {
            printf("  %d: P:$%08X\n", count, (unsigned int)bp->addr);
        } else {
            printf("  %d: $%08X\n", count, (unsigned int)bp->addr);
        }
        bp = bp->next;
        count++;
    }
}

// Set a logpoint at an address with category and level
static uint64_t cmd_logpoint(int argc, char *argv[]) {
    debug_t *debug = system_debug();

    // No args: list all logpoints (IMP-604)
    if (argc < 2) {
        if (debug)
            list_logpoints(debug);
        else
            printf("Debug not available\n");
        return 0;
    }

    // Handle "del" subcommand (IMP-604)
    if (strcmp(argv[1], "del") == 0) {
        if (!debug) {
            printf("Debug not available\n");
            return 0;
        }
        if (argc < 3) {
            printf("Usage: logpoint del <address|all>\n");
            return 0;
        }
        if (strcmp(argv[2], "all") == 0) {
            delete_all_logpoints(debug);
            printf("All logpoints deleted\n");
            return 0;
        }
        uint32_t addr;
        addr_space_t space;
        if (!parse_address(argv[2], &addr, &space)) {
            printf("Invalid address: %s\n", argv[2]);
            return 0;
        }
        if (delete_logpoint(debug, addr) == 0)
            printf("Logpoint at $%08X deleted\n", addr);
        else
            printf("No logpoint at $%08X\n", addr);
        return 0;
    }

    // Parse address or address range (required)
    uint32_t addr, end_addr;
    addr_space_t space;

    // Skip L:/P: prefix for separator search (colon in prefix would match length separator)
    int prefix_len = 0;
    if ((argv[1][0] == 'L' || argv[1][0] == 'l' || argv[1][0] == 'P' || argv[1][0] == 'p') && argv[1][1] == ':') {
        prefix_len = 2;
    }

    char *range_sep = strchr(argv[1] + prefix_len, '-');
    char *length_sep = strchr(argv[1] + prefix_len, ':');

    if (range_sep != NULL) {
        // Format: start-end (inclusive range)
        *range_sep = '\0';
        if (!parse_address(argv[1], &addr, &space)) {
            printf("Invalid start address: %s\n", argv[1]);
            return 0;
        }
        addr_space_t end_space;
        if (!parse_address(range_sep + 1, &end_addr, &end_space)) {
            printf("Invalid end address: %s\n", range_sep + 1);
            return 0;
        }
        if (end_addr < addr) {
            printf("End address must be >= start address\n");
            return 0;
        }
    } else if (length_sep != NULL) {
        // Format: start:length
        *length_sep = '\0';
        if (!parse_address(argv[1], &addr, &space)) {
            printf("Invalid start address: %s\n", argv[1]);
            return 0;
        }
        // Parse length as a numeric value (reuse parse_address, ignore space)
        addr_space_t len_space;
        uint32_t length;
        if (!parse_address(length_sep + 1, &length, &len_space)) {
            printf("Invalid length: %s\n", length_sep + 1);
            return 0;
        }
        if (length == 0) {
            printf("Length must be > 0\n");
            return 0;
        }
        end_addr = addr + length - 1; // inclusive end
    } else {
        // Single address
        if (!parse_address(argv[1], &addr, &space)) {
            printf("Invalid address: %s\n", argv[1]);
            return 0;
        }
        end_addr = addr; // single address: end == start
    }

    // Parse optional arguments - message and key=value pairs
    const char *message = NULL;
    const char *category_name = "logpoint";
    int level = 0;

    for (int i = 2; i < argc; i++) {
        char *arg = argv[i];
        // Check if this is a key=value pair
        char *equals = strchr(arg, '=');
        if (equals != NULL) {
            // Split into key and value
            *equals = '\0';
            char *key = arg;
            char *value = equals + 1;

            if (strcmp(key, "category") == 0) {
                category_name = value;
            } else if (strcmp(key, "level") == 0) {
                level = atoi(value);
                if (level < 0) {
                    printf("Invalid level: %s\n", value);
                    return 0;
                }
            } else {
                printf("Unknown parameter: %s\n", key);
                return 0;
            }
        } else if (message == NULL) {
            // First non-key=value argument is the message
            message = arg;
        } else {
            printf("Unexpected argument: %s\n", arg);
            return 0;
        }
    }

    // Get or register the log category
    log_category_t *category = log_get_category(category_name);
    if (category == NULL) {
        // Category doesn't exist yet, register it
        category = log_register_category(category_name);
        if (category == NULL) {
            printf("Failed to register log category: %s\n", category_name);
            return 0;
        }
    }

    // Set the logpoint (debug already obtained at function start)
    logpoint_t *lp = set_logpoint(debug, addr, end_addr, category, level);
    if (lp == NULL) {
        printf("Failed to set logpoint\n");
        return 0;
    }

    // Store message if provided
    if (message) {
        lp->message = strdup(message);
    }

    // Print confirmation with range info if applicable
    if (addr == end_addr) {
        // Single address logpoint
        if (message) {
            printf("logpoint set at $%08X: %s (category: %s, level: %d)\n", (unsigned int)addr, message, category_name,
                   level);
        } else {
            printf("logpoint set at $%08X (category: %s, level: %d)\n", (unsigned int)addr, category_name, level);
        }
    } else {
        // Range logpoint
        if (message) {
            printf("logpoint set at $%08X-$%08X: %s (category: %s, level: %d)\n", (unsigned int)addr,
                   (unsigned int)end_addr, message, category_name, level);
        } else {
            printf("logpoint set at $%08X-$%08X (category: %s, level: %d)\n", (unsigned int)addr,
                   (unsigned int)end_addr, category_name, level);
        }
    }

    return 0;
}
/*
static void cmd_step(struct config* config, int n)
{
    debugger_t* debug = &config->debugger;

    debug->step = n > 0 ? n : 1;

    scheduler_run(config->scheduler);
}*/

// Check if tracing is active (for log capture hook)
int debug_trace_is_active(void) {
    if (!system_is_initialized())
        return 0;
    debug_t *debug = system_debug();
    if (!debug)
        return 0;
    return debug->trace_entries != NULL;
}

// Check if debug functionality is engaged (breakpoints, logpoints, or tracing)
bool debug_active(debug_t *debug) {
    if (!debug)
        return false;
    return debug->breakpoints != NULL || debug->logpoints != NULL || debug->trace_buffer != NULL;
}

// Capture a log message to the trace buffer
void debug_trace_capture_log(const char *line) {
    if (!system_is_initialized() || !line)
        return;
    debug_t *debug = system_debug();
    if (!debug)
        return;

    // Only capture if trace entries buffer is active
    if (!debug->trace_entries)
        return;

    // Allocate log buffer on first use
    if (!debug->trace_log_buffer) {
        debug->trace_log_buffer_size = 0x100000; // 1M log entries
        debug->trace_log_buffer = calloc(debug->trace_log_buffer_size, sizeof(trace_log_msg_t));
        if (!debug->trace_log_buffer)
            return;
        debug->trace_log_head = 0;
        debug->trace_log_count = 0;
    }

    // Store log message in log buffer
    uint32_t log_idx = debug->trace_log_head;

    // Free old message if overwriting
    if (debug->trace_log_buffer[log_idx].text) {
        free(debug->trace_log_buffer[log_idx].text);
    }

    // Strip trailing newline if present
    size_t len = strlen(line);
    if (len > 0 && line[len - 1] == '\n') {
        debug->trace_log_buffer[log_idx].text = strndup(line, len - 1);
    } else {
        debug->trace_log_buffer[log_idx].text = strdup(line);
    }

    // Advance log buffer head
    debug->trace_log_head = (debug->trace_log_head + 1) % debug->trace_log_buffer_size;
    debug->trace_log_count++;

    // Add trace entry referencing this log
    uint32_t entry_idx = debug->trace_entries_head;
    debug->trace_entries[entry_idx].type = TRACE_ENTRY_LOG;
    debug->trace_entries[entry_idx].value = log_idx;

    // Advance entries head
    debug->trace_entries_head = (debug->trace_entries_head + 1) % debug->trace_entries_size;
    if (debug->trace_entries_head == debug->trace_entries_tail) {
        debug->trace_entries_tail = (debug->trace_entries_tail + 1) % debug->trace_entries_size;
    }
}

// Helper to add a PC entry to the trace
static void trace_add_pc_entry(debug_t *debug, uint32_t pc) {
    if (!debug->trace_entries)
        return;

    uint32_t entry_idx = debug->trace_entries_head;
    debug->trace_entries[entry_idx].type = TRACE_ENTRY_PC;
    debug->trace_entries[entry_idx].value = pc;

    debug->trace_entries_head = (debug->trace_entries_head + 1) % debug->trace_entries_size;
    if (debug->trace_entries_head == debug->trace_entries_tail) {
        debug->trace_entries_tail = (debug->trace_entries_tail + 1) % debug->trace_entries_size;
    }
}

// Helper to free trace log buffer entries
static void trace_free_log_buffer(debug_t *debug) {
    if (debug->trace_log_buffer) {
        for (uint32_t i = 0; i < debug->trace_log_buffer_size; i++) {
            if (debug->trace_log_buffer[i].text) {
                free(debug->trace_log_buffer[i].text);
                debug->trace_log_buffer[i].text = NULL;
            }
        }
        free(debug->trace_log_buffer);
        debug->trace_log_buffer = NULL;
    }
    debug->trace_log_head = 0;
    debug->trace_log_count = 0;
}

// Helper to output trace entries to a file or stdout
static int trace_output_entries(debug_t *debug, FILE *fp) {
    int count = 0;
    uint32_t i = debug->trace_entries_tail;

    while (i != debug->trace_entries_head) {
        trace_entry_t *entry = &debug->trace_entries[i];

        if (entry->type == TRACE_ENTRY_PC) {
            // Disassemble and output the instruction
            char buf[100];
            debugger_disasm(buf, entry->value);
            if (fp) {
                fprintf(fp, "%s\n", buf);
            } else {
                printf("%s\n", buf);
            }
            count++;
        } else if (entry->type == TRACE_ENTRY_LOG) {
            // Output the log message with indentation
            uint32_t log_idx = entry->value;
            if (debug->trace_log_buffer && log_idx < debug->trace_log_buffer_size) {
                const char *text = debug->trace_log_buffer[log_idx].text;
                if (text) {
                    if (fp) {
                        fprintf(fp, "          LOG: %s\n", text);
                    } else {
                        printf("          LOG: %s\n", text);
                    }
                }
            }
        }

        i = (i + 1) % debug->trace_entries_size;
    }

    return count;
}

static uint64_t cmd_trace(int argc, char *argv[]) {
    debug_t *debug = system_debug();

    if (argc < 2) {
        printf("Usage: trace <start|stop|show [filename]>\n");
        printf("  trace show          - display trace to stdout\n");
        printf("  trace show <file>   - save trace to file\n");
        printf("  Log entries from enabled categories are captured and shown after their triggering instruction.\n");
        return 0;
    }

    const char *s = argv[1];

    if (strcmp(s, "start") == 0) {
        // Allocate legacy trace buffer (for backward compatibility)
        if (debug->trace_buffer == NULL) {
            debug->trace_buffer_size = 0x1000000;
            debug->trace_buffer = malloc(4 * debug->trace_buffer_size);
        }
        debug->trace_head = 0;
        debug->trace_tail = 0;
        debug->trace_size = 0;

        // Allocate new trace entries buffer
        if (debug->trace_entries == NULL) {
            debug->trace_entries_size = 0x2000000; // 32M entries (PC + log)
            debug->trace_entries = calloc(debug->trace_entries_size, sizeof(trace_entry_t));
        }
        debug->trace_entries_head = 0;
        debug->trace_entries_tail = 0;

        // Log buffer will be allocated on first log capture
        trace_free_log_buffer(debug);

        printf("Trace started (log capture enabled).\n");
    } else if (strcmp(s, "stop") == 0) {
        // Free legacy trace buffer
        free(debug->trace_buffer);
        debug->trace_buffer = NULL;

        // Free trace entries buffer
        free(debug->trace_entries);
        debug->trace_entries = NULL;
        debug->trace_entries_head = 0;
        debug->trace_entries_tail = 0;

        // Free log buffer
        trace_free_log_buffer(debug);

        printf("Trace stopped.\n");
    } else if (strcmp(s, "show") == 0) {
        // Use new trace entries if available, otherwise fall back to legacy
        if (debug->trace_entries) {
            if (argc >= 3) {
                // Save trace to file
                const char *filename = argv[2];
                FILE *fp = fopen(filename, "w");
                if (fp == NULL) {
                    printf("Error: Failed to open file '%s' for writing\n", filename);
                    return 0;
                }

                int count = trace_output_entries(debug, fp);
                fclose(fp);
                printf("Trace saved to '%s' (%d instruction entries)\n", filename, count);
            } else {
                // Display trace to stdout
                trace_output_entries(debug, NULL);
            }
        } else if (debug->trace_buffer) {
            // Legacy trace buffer display
            if (argc >= 3) {
                const char *filename = argv[2];
                FILE *fp = fopen(filename, "w");
                if (fp == NULL) {
                    printf("Error: Failed to open file '%s' for writing\n", filename);
                    return 0;
                }

                int count = 0;
                for (int i = debug->trace_tail; i != debug->trace_head; i = (i + 1) % debug->trace_buffer_size) {
                    char buf[100];
                    debugger_disasm(buf, debug->trace_buffer[i]);
                    fprintf(fp, "%s\n", buf);
                    count++;
                }

                fclose(fp);
                printf("Trace saved to '%s' (%d entries)\n", filename, count);
            } else {
                for (int i = debug->trace_tail; i != debug->trace_head; i = (i + 1) % debug->trace_buffer_size) {
                    char buf[100];
                    debugger_disasm(buf, debug->trace_buffer[i]);
                    printf("%s\n", buf);
                }
            }
        } else {
            printf("No trace data available. Use 'trace start' first.\n");
        }
    } else {
        printf("Invalid trace command: %s\n", s);
        printf("Usage: trace <start|stop|show [filename]>\n");
        printf("  trace show          - display trace to stdout\n");
        printf("  trace show <file>   - save trace to file\n");
    }

    return 0;
}

static uint64_t cmd_td(int argc, char *argv[]) {
    cpu_t *cpu = system_cpu();
    if (!cpu) {
        printf("CPU not available\n");
        return 0;
    }

    // CPU/Addressing/MMU header line
    bool is_24bit = (g_address_mask == 0x00FFFFFF);
    if (g_mmu) {
        printf("CPU: 68030  Addressing: %s  MMU: %s", is_24bit ? "24-bit" : "32-bit",
               g_mmu->enabled ? "enabled" : "disabled");
        if (g_mmu->enabled)
            printf(" (TC=$%08X)", g_mmu->tc);
        printf("\n");
    } else {
        printf("CPU: 68000  Addressing: %s  MMU: none\n", is_24bit ? "24-bit" : "32-bit");
    }

    printf("D0 = $%08X    A0 = $%08X\n", (unsigned int)cpu_get_dn(cpu, 0), (unsigned int)cpu_get_an(cpu, 0));
    printf("D1 = $%08X    A1 = $%08X\n", (unsigned int)cpu_get_dn(cpu, 1), (unsigned int)cpu_get_an(cpu, 1));
    printf("D2 = $%08X    A2 = $%08X\n", (unsigned int)cpu_get_dn(cpu, 2), (unsigned int)cpu_get_an(cpu, 2));
    printf("D3 = $%08X    A3 = $%08X\n", (unsigned int)cpu_get_dn(cpu, 3), (unsigned int)cpu_get_an(cpu, 3));
    printf("D4 = $%08X    A4 = $%08X\n", (unsigned int)cpu_get_dn(cpu, 4), (unsigned int)cpu_get_an(cpu, 4));
    printf("D5 = $%08X    A5 = $%08X\n", (unsigned int)cpu_get_dn(cpu, 5), (unsigned int)cpu_get_an(cpu, 5));
    printf("D6 = $%08X    A6 = $%08X\n", (unsigned int)cpu_get_dn(cpu, 6), (unsigned int)cpu_get_an(cpu, 6));
    printf("D7 = $%08X    A7 = $%08X\n", (unsigned int)cpu_get_dn(cpu, 7), (unsigned int)cpu_get_an(cpu, 7));

    // PC, SR with decoded fields, USP/SSP (IMP-401, IMP-405)
    uint16_t sr = cpu_get_sr(cpu);
    int s_bit = (sr >> 13) & 1;
    int im = (sr >> 8) & 7;
    int t_bit = (sr >> 15) & 1;
    int x = (sr & cpu_ccr_x) ? 1 : 0;
    int n = (sr & cpu_ccr_n) ? 1 : 0;
    int z = (sr & cpu_ccr_z) ? 1 : 0;
    int v = (sr & cpu_ccr_v) ? 1 : 0;
    int cc = (sr & cpu_ccr_c) ? 1 : 0;
    printf("PC = $%08X    SR = $%04X  (S=%d, IM=%d, T=%d, XNZVC=%d%d%d%d%d)\n", (unsigned int)cpu_get_pc(cpu), sr,
           s_bit, im, t_bit, x, n, z, v, cc);
    printf("USP = $%08X   SSP = $%08X\n", (unsigned int)cpu_get_usp(cpu), (unsigned int)cpu_get_ssp(cpu));

    return 0;
}

// Helper function to convert string to uppercase (for case-insensitive comparisons)
static void str_to_upper(char *dest, const char *src) {
    while (*src) {
        *dest++ = (*src >= 'a' && *src <= 'z') ? (*src - 32) : *src;
        src++;
    }
    *dest = '\0';
}

static uint64_t cmd_set(int argc, char *argv[]) {
    if (argc != 3) {
        printf("Usage: set <register|address> <value>\n");
        printf("  Registers: D0-D7, A0-A7, PC, SP, SSP, USP, SR, CCR\n");
        printf("  Condition codes: C, V, Z, N, X\n");
        printf("  Memory: <address>.b|.w|.l <value>\n");
        return 0;
    }

    cpu_t *cpu = system_cpu();
    if (!cpu) {
        printf("CPU not available\n");
        return 0;
    }
    char target[64];
    str_to_upper(target, argv[1]);

    // Parse the value (support $ prefix for Motorola hex)
    const char *val_str = argv[2];
    if (*val_str == '$')
        val_str++;
    char *endptr;
    uint32_t value = (uint32_t)strtoul(val_str, &endptr,
                                       (*val_str == '0' && (*(val_str + 1) == 'x' || *(val_str + 1) == 'X')) ? 0 : 16);
    if (*endptr != '\0') {
        printf("Invalid value: %s\n", argv[2]);
        return 0;
    }

    // Data registers (D0-D7)
    if (target[0] == 'D' && target[1] >= '0' && target[1] <= '7' && target[2] == '\0') {
        int reg = target[1] - '0';
        cpu_set_dn(cpu, reg, value);
        printf("D%d = $%08X\n", reg, value);
        return 0;
    }

    // Address registers (A0-A7)
    if (target[0] == 'A' && target[1] >= '0' && target[1] <= '7' && target[2] == '\0') {
        int reg = target[1] - '0';
        cpu_set_an(cpu, reg, value);
        printf("A%d = $%08X\n", reg, value);
        return 0;
    }

    // Program Counter
    if (strcmp(target, "PC") == 0) {
        cpu_set_pc(cpu, value);
        printf("PC = $%08X\n", value);
        return 0;
    }

    // Stack Pointer (alias for A7)
    if (strcmp(target, "SP") == 0) {
        cpu_set_an(cpu, 7, value);
        printf("SP = $%08X\n", value);
        return 0;
    }

    // Supervisor Stack Pointer
    if (strcmp(target, "SSP") == 0) {
        cpu_set_ssp(cpu, value);
        printf("SSP = $%08X\n", value);
        return 0;
    }

    // User Stack Pointer
    if (strcmp(target, "USP") == 0) {
        cpu_set_usp(cpu, value);
        printf("USP = $%08X\n", value);
        return 0;
    }

    // Status Register
    if (strcmp(target, "SR") == 0) {
        cpu_set_sr(cpu, (uint16_t)value);
        printf("SR = $%04X\n", (uint16_t)value);
        return 0;
    }

    // Condition Code Register
    if (strcmp(target, "CCR") == 0) {
        uint16_t sr = cpu_get_sr(cpu);
        sr = (sr & ~cpu_ccr_mask) | (value & cpu_ccr_mask);
        cpu_set_sr(cpu, sr);
        printf("CCR = $%02X\n", (uint8_t)(value & cpu_ccr_mask));
        return 0;
    }

    // Individual condition code flags
    if (strcmp(target, "C") == 0) {
        uint16_t sr = cpu_get_sr(cpu);
        if (value & 1)
            sr |= cpu_ccr_c;
        else
            sr &= ~cpu_ccr_c;
        cpu_set_sr(cpu, sr);
        printf("C = %d\n", (value & 1));
        return 0;
    }

    if (strcmp(target, "V") == 0) {
        uint16_t sr = cpu_get_sr(cpu);
        if (value & 1)
            sr |= cpu_ccr_v;
        else
            sr &= ~cpu_ccr_v;
        cpu_set_sr(cpu, sr);
        printf("V = %d\n", (value & 1));
        return 0;
    }

    if (strcmp(target, "Z") == 0) {
        uint16_t sr = cpu_get_sr(cpu);
        if (value & 1)
            sr |= cpu_ccr_z;
        else
            sr &= ~cpu_ccr_z;
        cpu_set_sr(cpu, sr);
        printf("Z = %d\n", (value & 1));
        return 0;
    }

    if (strcmp(target, "N") == 0) {
        uint16_t sr = cpu_get_sr(cpu);
        if (value & 1)
            sr |= cpu_ccr_n;
        else
            sr &= ~cpu_ccr_n;
        cpu_set_sr(cpu, sr);
        printf("N = %d\n", (value & 1));
        return 0;
    }

    if (strcmp(target, "X") == 0) {
        uint16_t sr = cpu_get_sr(cpu);
        if (value & 1)
            sr |= cpu_ccr_x;
        else
            sr &= ~cpu_ccr_x;
        cpu_set_sr(cpu, sr);
        printf("X = %d\n", (value & 1));
        return 0;
    }

    // Master Stack Pointer (68030 only, IMP-406)
    if (strcmp(target, "MSP") == 0) {
        if (cpu->cpu_model != CPU_MODEL_68030) {
            printf("MSP is only available on 68030\n");
            return 0;
        }
        cpu->msp = value;
        printf("MSP = $%08X\n", value);
        return 0;
    }

    // FPU control registers (IMP-403)
    if (strcmp(target, "FPCR") == 0) {
        fpu_state_t *fpu = (fpu_state_t *)cpu->fpu;
        if (!fpu) {
            printf("FPU not available\n");
            return 0;
        }
        fpu->fpcr = value;
        printf("FPCR = $%08X\n", value);
        return 0;
    }
    if (strcmp(target, "FPSR") == 0) {
        fpu_state_t *fpu = (fpu_state_t *)cpu->fpu;
        if (!fpu) {
            printf("FPU not available\n");
            return 0;
        }
        fpu->fpsr = value;
        printf("FPSR = $%08X\n", value);
        return 0;
    }
    if (strcmp(target, "FPIAR") == 0) {
        fpu_state_t *fpu = (fpu_state_t *)cpu->fpu;
        if (!fpu) {
            printf("FPU not available\n");
            return 0;
        }
        fpu->fpiar = value;
        printf("FPIAR = $%08X\n", value);
        return 0;
    }

    // Individual MMU registers (IMP-404)
    if (strcmp(target, "TC") == 0) {
        if (!g_mmu) {
            printf("MMU not present\n");
            return 0;
        }
        g_mmu->tc = value;
        printf("TC = $%08X\n", value);
        return 0;
    }
    if (strcmp(target, "TT0") == 0) {
        if (!g_mmu) {
            printf("MMU not present\n");
            return 0;
        }
        g_mmu->tt0 = value;
        printf("TT0 = $%08X\n", value);
        return 0;
    }
    if (strcmp(target, "TT1") == 0) {
        if (!g_mmu) {
            printf("MMU not present\n");
            return 0;
        }
        g_mmu->tt1 = value;
        printf("TT1 = $%08X\n", value);
        return 0;
    }

    // Memory addresses with size specifiers (.b, .w, .l)
    char *size_spec = strchr(target, '.');
    if (size_spec != NULL) {
        *size_spec = '\0';
        size_spec++;

        // Parse the address from target (supports $, 0x, and bare hex)
        uint32_t addr;
        addr_space_t space;
        if (!parse_address(target, &addr, &space)) {
            printf("Invalid address: %s\n", target);
            return 0;
        }

        if (strcmp(size_spec, "B") == 0) {
            memory_write_uint8(addr, (uint8_t)value);
            printf("[$%08X].b = $%02X\n", addr, (uint8_t)value);
            return 0;
        } else if (strcmp(size_spec, "W") == 0) {
            memory_write_uint16(addr, (uint16_t)value);
            printf("[$%08X].w = $%04X\n", addr, (uint16_t)value);
            return 0;
        } else if (strcmp(size_spec, "L") == 0) {
            memory_write_uint32(addr, value);
            printf("[$%08X].l = $%08X\n", addr, value);
            return 0;
        } else {
            printf("Invalid size specifier: .%s (use .b, .w, or .l)\n", size_spec);
            return 0;
        }
    }

    printf("Unknown target: %s\n", target);
    printf("Usage: set <register|address> <value>\n");
    printf("  Registers: D0-D7, A0-A7, PC, SP, SSP, USP, SR, CCR\n");
    printf("  FPU: FPCR, FPSR, FPIAR   MMU: TC, TT0, TT1   68030: MSP\n");
    printf("  Condition codes: C, V, Z, N, X\n");
    printf("  Memory: <address>.b|.w|.l <value>\n");
    printf("  Values are hex by default (42 = $42). Use 0d prefix for decimal (0d66).\n");
    return 0;
}

// ────────────────────────────────────────────────────────────────────────────
// Screenshot command - save emulated screen as PNG
// ────────────────────────────────────────────────────────────────────────────

// Screen dimensions (Macintosh Plus)
#define SCREEN_WIDTH  512
#define SCREEN_HEIGHT 342
#define FB_BYTES      (SCREEN_WIDTH * SCREEN_HEIGHT / 8)

// CRC32 table for PNG chunk checksums
static uint32_t crc32_table[256];
static int crc32_table_init = 0;

// Initialize CRC32 lookup table
static void init_crc32_table(void) {
    if (crc32_table_init)
        return;
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) {
            if (c & 1)
                c = 0xedb88320 ^ (c >> 1);
            else
                c = c >> 1;
        }
        crc32_table[n] = c;
    }
    crc32_table_init = 1;
}

// Write 32-bit big-endian value to buffer
static void write_be32(uint8_t *p, uint32_t v) {
    p[0] = (v >> 24) & 0xff;
    p[1] = (v >> 16) & 0xff;
    p[2] = (v >> 8) & 0xff;
    p[3] = v & 0xff;
}

// Write a PNG chunk to file
static int write_png_chunk(FILE *fp, const char *type, const uint8_t *data, uint32_t len) {
    uint8_t header[8];
    // Write length (big-endian)
    write_be32(header, len);
    // Write type
    memcpy(header + 4, type, 4);
    if (fwrite(header, 1, 8, fp) != 8)
        return -1;

    // Write data (if any)
    if (len > 0 && data) {
        if (fwrite(data, 1, len, fp) != len)
            return -1;
    }

    // Compute CRC over type + data
    uint32_t crc = 0xffffffff;
    for (int i = 0; i < 4; i++) {
        crc = crc32_table[(crc ^ header[4 + i]) & 0xff] ^ (crc >> 8);
    }
    for (uint32_t i = 0; i < len; i++) {
        crc = crc32_table[(crc ^ data[i]) & 0xff] ^ (crc >> 8);
    }
    crc ^= 0xffffffff;

    // Write CRC (big-endian)
    uint8_t crc_buf[4];
    write_be32(crc_buf, crc);
    if (fwrite(crc_buf, 1, 4, fp) != 4)
        return -1;

    return 0;
}

// Adler-32 checksum for zlib
static uint32_t adler32(const uint8_t *data, size_t len) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < len; i++) {
        a = (a + data[i]) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

// Calculate a simple checksum of the framebuffer for fast screen comparison
static uint32_t framebuffer_checksum(const uint8_t *fb) {
    uint32_t checksum = 0;
    for (int i = 0; i < FB_BYTES; i++) {
        checksum = checksum * 31 + fb[i];
    }
    return checksum;
}

// Calculate checksum for a region of the framebuffer (top, left, bottom, right)
// Region is specified in pixels; checksum operates on the 1-bit packed data
static uint32_t framebuffer_region_checksum(const uint8_t *fb, int top, int left, int bottom, int right) {
    uint32_t checksum = 0;
    int width = right - left;
    int height = bottom - top;
    (void)width;
    (void)height;

    // Process each row in the region
    for (int y = top; y < bottom; y++) {
        // Process each pixel in the row, packing into bytes
        for (int x = left; x < right; x++) {
            int byte_idx = y * (SCREEN_WIDTH / 8) + (x / 8);
            int bit_idx = 7 - (x % 8); // MSB first
            int bit = (fb[byte_idx] >> bit_idx) & 1;

            // Pack bits into a byte and add to checksum when we have 8 bits
            // or at end of row
            int rel_x = x - left;
            int byte_bit = 7 - (rel_x % 8);
            static uint8_t accum_byte = 0;
            if (byte_bit == 7)
                accum_byte = 0; // Start new byte
            if (bit)
                accum_byte |= (1 << byte_bit);
            if (byte_bit == 0 || x == right - 1) {
                checksum = checksum * 31 + accum_byte;
                accum_byte = 0;
            }
        }
    }
    return checksum;
}

// ────────────────────────────────────────────────────────────────────────────
// PNG reading for --match comparison
// ────────────────────────────────────────────────────────────────────────────

// Read 32-bit big-endian value from buffer
static uint32_t read_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

// Simple inflate implementation for PNG stored blocks only
// Returns decompressed data or NULL on error
static uint8_t *inflate_stored_only(const uint8_t *data, size_t data_len, size_t *out_len) {
    if (data_len < 6)
        return NULL; // Need zlib header + at least one block

    // Skip zlib header (2 bytes)
    size_t pos = 2;

    // First pass: calculate total output size
    size_t total_size = 0;
    size_t scan_pos = pos;
    while (scan_pos < data_len - 4) { // Leave room for adler32
        uint8_t bfinal_btype = data[scan_pos++];
        int btype = (bfinal_btype >> 1) & 0x03;
        int bfinal = bfinal_btype & 0x01;

        if (btype != 0) {
            // Not a stored block - we only support stored blocks
            return NULL;
        }

        if (scan_pos + 4 > data_len)
            return NULL;
        uint16_t len = data[scan_pos] | ((uint16_t)data[scan_pos + 1] << 8);
        scan_pos += 4; // Skip len and nlen

        total_size += len;
        scan_pos += len;

        if (bfinal)
            break;
    }

    // Allocate output buffer
    uint8_t *output = malloc(total_size);
    if (!output)
        return NULL;

    // Second pass: extract data
    size_t out_pos = 0;
    while (pos < data_len - 4) {
        uint8_t bfinal_btype = data[pos++];
        int bfinal = bfinal_btype & 0x01;

        uint16_t len = data[pos] | ((uint16_t)data[pos + 1] << 8);
        pos += 4; // Skip len and nlen

        memcpy(output + out_pos, data + pos, len);
        out_pos += len;
        pos += len;

        if (bfinal)
            break;
    }

    *out_len = total_size;
    return output;
}

// Load a PNG file and extract framebuffer (1-bit packed, same format as emulator)
// Returns 0 on success, -1 on error
// Supports both stored-block PNGs (our format) and standard deflate PNGs
static int load_png_to_framebuffer(const char *filename, uint8_t *fb_out) {
    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        printf("Error: Cannot open '%s' for reading.\n", filename);
        return -1;
    }

    // Read entire file
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    uint8_t *file_data = malloc(file_size);
    if (!file_data) {
        fclose(fp);
        printf("Error: Out of memory.\n");
        return -1;
    }

    if (fread(file_data, 1, file_size, fp) != (size_t)file_size) {
        free(file_data);
        fclose(fp);
        printf("Error: Failed to read file.\n");
        return -1;
    }
    fclose(fp);

    // Check PNG signature
    static const uint8_t png_sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    if (file_size < 8 || memcmp(file_data, png_sig, 8) != 0) {
        free(file_data);
        printf("Error: Not a valid PNG file.\n");
        return -1;
    }

    // Parse chunks to find IHDR and IDAT
    size_t pos = 8;
    int width = 0, height = 0, bit_depth = 0, color_type = 0;
    uint8_t *idat_data = NULL;
    size_t idat_len = 0;
    size_t idat_capacity = 0;

    while (pos + 12 <= (size_t)file_size) {
        uint32_t chunk_len = read_be32(file_data + pos);
        char chunk_type[5];
        memcpy(chunk_type, file_data + pos + 4, 4);
        chunk_type[4] = '\0';

        if (strcmp(chunk_type, "IHDR") == 0 && chunk_len >= 13) {
            width = read_be32(file_data + pos + 8);
            height = read_be32(file_data + pos + 12);
            bit_depth = file_data[pos + 16];
            color_type = file_data[pos + 17];
        } else if (strcmp(chunk_type, "IDAT") == 0) {
            // Append IDAT data
            if (idat_len + chunk_len > idat_capacity) {
                idat_capacity = (idat_len + chunk_len) * 2;
                uint8_t *new_idat = realloc(idat_data, idat_capacity);
                if (!new_idat) {
                    free(idat_data);
                    free(file_data);
                    printf("Error: Out of memory.\n");
                    return -1;
                }
                idat_data = new_idat;
            }
            memcpy(idat_data + idat_len, file_data + pos + 8, chunk_len);
            idat_len += chunk_len;
        } else if (strcmp(chunk_type, "IEND") == 0) {
            break;
        }

        pos += 12 + chunk_len; // length + type + data + crc
    }

    free(file_data);

    // Validate dimensions
    if (width != SCREEN_WIDTH || height != SCREEN_HEIGHT) {
        free(idat_data);
        printf("Error: PNG dimensions %dx%d don't match screen %dx%d.\n", width, height, SCREEN_WIDTH, SCREEN_HEIGHT);
        return -1;
    }

    if (!idat_data || idat_len == 0) {
        free(idat_data);
        printf("Error: No image data in PNG.\n");
        return -1;
    }

    // Decompress IDAT data (stored blocks only)
    size_t raw_size = 0;
    uint8_t *raw_data = inflate_stored_only(idat_data, idat_len, &raw_size);
    free(idat_data);

    if (!raw_data) {
        printf("Error: Failed to decompress PNG (only stored-block PNGs supported).\n");
        return -1;
    }

    // Calculate expected size based on color type
    size_t bytes_per_pixel;
    if (color_type == 6) {
        bytes_per_pixel = 4; // RGBA
    } else if (color_type == 2) {
        bytes_per_pixel = 3; // RGB
    } else if (color_type == 0) {
        bytes_per_pixel = 1; // Grayscale
    } else {
        free(raw_data);
        printf("Error: Unsupported PNG color type %d.\n", color_type);
        return -1;
    }

    size_t row_size = 1 + width * bytes_per_pixel; // 1 filter byte + pixel data
    size_t expected_size = row_size * height;

    if (raw_size != expected_size) {
        free(raw_data);
        printf("Error: PNG data size mismatch (got %zu, expected %zu).\n", raw_size, expected_size);
        return -1;
    }

    // Convert RGBA/RGB/Grayscale back to 1-bit packed framebuffer
    memset(fb_out, 0, FB_BYTES);
    for (int y = 0; y < height; y++) {
        uint8_t *row = raw_data + y * row_size + 1; // Skip filter byte
        for (int x = 0; x < width; x++) {
            // Get pixel brightness
            uint8_t v;
            if (color_type == 6) {
                v = row[x * 4]; // Use R channel from RGBA
            } else if (color_type == 2) {
                v = row[x * 3]; // Use R channel from RGB
            } else {
                v = row[x]; // Grayscale
            }

            // Convert to 1-bit: bright = white (0), dark = black (1)
            int bit = (v < 128) ? 1 : 0;

            // Pack into framebuffer
            int byte_idx = y * (SCREEN_WIDTH / 8) + (x / 8);
            int bit_idx = 7 - (x % 8); // MSB first
            if (bit) {
                fb_out[byte_idx] |= (1 << bit_idx);
            }
        }
    }

    free(raw_data);
    return 0;
}

// Compare current framebuffer with a reference PNG file
// Returns 0 if match, 1 if mismatch, -1 on error
static int match_framebuffer_with_png(const uint8_t *fb, const char *filename) {
    uint8_t *ref_fb = malloc(FB_BYTES);
    if (!ref_fb) {
        printf("Error: Out of memory.\n");
        return -1;
    }

    if (load_png_to_framebuffer(filename, ref_fb) < 0) {
        free(ref_fb);
        return -1;
    }

    // Compare framebuffers
    int match = (memcmp(fb, ref_fb, FB_BYTES) == 0);
    free(ref_fb);

    return match ? 0 : 1;
}

// Save the current emulated screen as a PNG file, or compute checksum
static uint64_t cmd_screenshot(int argc, char *argv[]) {
    // Check framebuffer availability first
    const uint8_t *fb = system_framebuffer();
    if (!fb) {
        printf("Error: Framebuffer not available.\n");
        return 0;
    }

    // Handle --checksum / -c flag with optional region (top left bottom right)
    if (argc >= 2 && (strcmp(argv[1], "--checksum") == 0 || strcmp(argv[1], "-c") == 0)) {
        uint32_t checksum;
        if (argc >= 6) {
            // Region specified: screenshot --checksum top left bottom right
            int top = atoi(argv[2]);
            int left = atoi(argv[3]);
            int bottom = atoi(argv[4]);
            int right = atoi(argv[5]);
            // Validate bounds
            if (top < 0 || left < 0 || bottom <= top || right <= left || bottom > SCREEN_HEIGHT ||
                right > SCREEN_WIDTH) {
                printf("Error: Invalid region bounds (0,0)-(512,342)\n");
                return 0;
            }
            checksum = framebuffer_region_checksum(fb, top, left, bottom, right);
            printf("Region checksum (%d,%d)-(%d,%d): 0x%08x\n", top, left, bottom, right, checksum);
        } else {
            // Full screen checksum
            checksum = framebuffer_checksum(fb);
            printf("Screen checksum: 0x%08x\n", checksum);
        }
        return checksum;
    }

    // Handle --match / -m flag to compare screen with reference PNG
    if (argc >= 3 && (strcmp(argv[1], "--match") == 0 || strcmp(argv[1], "-m") == 0)) {
        const char *ref_filename = argv[2];
        int result = match_framebuffer_with_png(fb, ref_filename);
        if (result < 0) {
            printf("MATCH FAILED: Error loading reference image.\n");
            return 2; // Error
        } else if (result == 0) {
            printf("MATCH OK: Screen matches '%s'.\n", ref_filename);
            return 0; // Match
        } else {
            printf("MATCH FAILED: Screen does not match '%s'.\n", ref_filename);
            return 1; // Mismatch
        }
    }

    if (argc < 2) {
        printf("Usage: screenshot <filename.png>\n");
        printf("       screenshot --checksum|-c [top left bottom right]\n");
        printf("       screenshot --match|-m <reference.png>\n");
        printf("  Save the current emulated screen (512x342) as a PNG file,\n");
        printf("  compute a checksum for fast screen comparison,\n");
        printf("  or compare screen with a reference PNG file.\n");
        printf("  Optional region bounds for checksum (default: full screen).\n");
        return 0;
    }

    const char *filename = argv[1];

    // Initialize CRC table
    init_crc32_table();

    // Open output file
    FILE *fp = fopen(filename, "wb");
    if (!fp) {
        printf("Error: Cannot open file '%s' for writing.\n", filename);
        return 0;
    }

    // PNG signature
    static const uint8_t png_sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    if (fwrite(png_sig, 1, 8, fp) != 8)
        goto write_error;

    // IHDR chunk: width, height, bit depth, color type, compression, filter, interlace
    // bit depth = 8, color type = 6 (RGBA)
    uint8_t ihdr[13];
    write_be32(ihdr, SCREEN_WIDTH);
    write_be32(ihdr + 4, SCREEN_HEIGHT);
    ihdr[8] = 8; // bit depth (8 bits per channel)
    ihdr[9] = 6; // color type (RGBA)
    ihdr[10] = 0; // compression method (deflate)
    ihdr[11] = 0; // filter method
    ihdr[12] = 0; // interlace method (none)
    if (write_png_chunk(fp, "IHDR", ihdr, 13) < 0)
        goto write_error;

    // Prepare uncompressed image data with filter bytes
    // Each row: 1 filter byte (0 = none) + SCREEN_WIDTH * 4 bytes (RGBA)
    size_t row_size = 1 + SCREEN_WIDTH * 4;
    size_t raw_size = row_size * SCREEN_HEIGHT;
    uint8_t *raw_data = malloc(raw_size);
    if (!raw_data) {
        printf("Error: Out of memory.\n");
        fclose(fp);
        return 0;
    }

    // Convert 1-bit packed framebuffer to RGBA
    // In Mac framebuffer: 1 = black, 0 = white
    for (int y = 0; y < SCREEN_HEIGHT; y++) {
        uint8_t *row = raw_data + y * row_size;
        row[0] = 0; // filter byte: none
        for (int x = 0; x < SCREEN_WIDTH; x++) {
            int byte_idx = y * (SCREEN_WIDTH / 8) + (x / 8);
            int bit_idx = 7 - (x % 8); // MSB first
            int bit = (fb[byte_idx] >> bit_idx) & 1;
            // bit=1 means black (0), bit=0 means white (255)
            uint8_t v = bit ? 0 : 255;
            uint8_t *pixel = row + 1 + x * 4;
            pixel[0] = v; // R
            pixel[1] = v; // G
            pixel[2] = v; // B
            pixel[3] = 255; // A (fully opaque)
        }
    }

    // Create "uncompressed" zlib stream using stored blocks
    // zlib header (2 bytes) + stored blocks + adler32 (4 bytes)
    // Each stored block: 1 byte header + 2 bytes len + 2 bytes nlen + data
    // Max block size is 65535 bytes

    // Calculate number of blocks needed
    size_t max_block = 65535;
    size_t num_blocks = (raw_size + max_block - 1) / max_block;
    // zlib overhead: 2 (header) + 5*num_blocks (block headers) + 4 (adler32)
    size_t zlib_size = 2 + raw_size + 5 * num_blocks + 4;

    uint8_t *zlib_data = malloc(zlib_size);
    if (!zlib_data) {
        printf("Error: Out of memory.\n");
        free(raw_data);
        fclose(fp);
        return 0;
    }

    size_t zpos = 0;
    // zlib header: CMF=0x78 (deflate, 32K window), FLG=0x01 (no dict, check bits)
    zlib_data[zpos++] = 0x78;
    zlib_data[zpos++] = 0x01;

    // Write stored blocks
    size_t remaining = raw_size;
    size_t src_pos = 0;
    while (remaining > 0) {
        size_t block_len = (remaining > max_block) ? max_block : remaining;
        int is_final = (remaining <= max_block) ? 1 : 0;
        // Block header: BFINAL (1 bit) + BTYPE (2 bits) = 0b000 or 0b001 for stored
        zlib_data[zpos++] = is_final ? 0x01 : 0x00;
        // LEN (2 bytes, little-endian)
        zlib_data[zpos++] = block_len & 0xff;
        zlib_data[zpos++] = (block_len >> 8) & 0xff;
        // NLEN (one's complement of LEN)
        zlib_data[zpos++] = (~block_len) & 0xff;
        zlib_data[zpos++] = ((~block_len) >> 8) & 0xff;
        // Data
        memcpy(zlib_data + zpos, raw_data + src_pos, block_len);
        zpos += block_len;
        src_pos += block_len;
        remaining -= block_len;
    }

    // Adler-32 checksum of uncompressed data (big-endian)
    uint32_t adler = adler32(raw_data, raw_size);
    write_be32(zlib_data + zpos, adler);
    zpos += 4;

    free(raw_data);

    // Write IDAT chunk
    if (write_png_chunk(fp, "IDAT", zlib_data, (uint32_t)zpos) < 0) {
        free(zlib_data);
        goto write_error;
    }
    free(zlib_data);

    // Write IEND chunk (empty)
    if (write_png_chunk(fp, "IEND", NULL, 0) < 0)
        goto write_error;

    fclose(fp);
    printf("Screenshot saved to '%s' (%dx%d).\n", filename, SCREEN_WIDTH, SCREEN_HEIGHT);
    return 0;

write_error:
    printf("Error: Failed to write to file '%s'.\n", filename);
    fclose(fp);
    return 0;
}

// Address display mode command: addrmode [auto|expanded|collapsed]
static uint64_t cmd_addrmode(int argc, char *argv[]) {
    if (argc < 2) {
        // Show current mode
        const char *mode_str;
        switch (g_addr_display_mode) {
        case ADDR_DISPLAY_AUTO:
            mode_str = "auto";
            break;
        case ADDR_DISPLAY_EXPANDED:
            mode_str = "expanded";
            break;
        case ADDR_DISPLAY_COLLAPSED:
            mode_str = "collapsed";
            break;
        default:
            mode_str = "unknown";
            break;
        }
        printf("Address display: %s\n", mode_str);
        return 0;
    }

    if (strcmp(argv[1], "auto") == 0) {
        g_addr_display_mode = ADDR_DISPLAY_AUTO;
        printf("Address display: auto (collapsed when MMU off, expanded when MMU on)\n");
    } else if (strcmp(argv[1], "expanded") == 0) {
        g_addr_display_mode = ADDR_DISPLAY_EXPANDED;
        printf("Address display: expanded (always show L: and P:)\n");
    } else if (strcmp(argv[1], "collapsed") == 0) {
        g_addr_display_mode = ADDR_DISPLAY_COLLAPSED;
        printf("Address display: collapsed (omit P: when L = P)\n");
    } else {
        printf("Usage: addrmode [auto|expanded|collapsed]\n");
    }
    return 0;
}

// Translate command: translate <address> — show MMU translation details
static uint64_t cmd_translate(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: translate <address>  – show MMU translation for a logical address\n");
        return 0;
    }

    uint32_t addr;
    addr_space_t space;
    if (!parse_address(argv[1], &addr, &space)) {
        printf("Invalid address: %s\n", argv[1]);
        return 0;
    }

    if (space == ADDR_PHYSICAL) {
        printf("P:$%08X is already a physical address\n", addr);
        return 0;
    }

    bool is_identity = true;
    bool tt_hit = false;
    bool valid = true;
    uint32_t phys = debug_translate_address(addr, &is_identity, &tt_hit, &valid);

    if (!g_mmu || !g_mmu->enabled) {
        printf("L:$%08X -> P:$%08X  (no MMU)\n", addr, phys);
        return 0;
    }

    if (tt_hit) {
        printf("L:$%08X -> P:$%08X  (TT, transparent translation)\n", addr, phys);
    } else if (!valid) {
        printf("L:$%08X -> INVALID  (descriptor invalid)\n", addr);
    } else {
        printf("L:$%08X -> P:$%08X  (page table%s)\n", addr, phys, is_identity ? ", identity" : "");
    }
    return 0;
}

// ============================================================================
// FPU register dump (IMP-402)
// ============================================================================

// Convert float80 to approximate double for display
static double fp80_to_double(float80_reg_t f) {
    if (fp80_is_zero(f))
        return FP80_SIGN(f) ? -0.0 : 0.0;
    if (fp80_is_inf(f))
        return FP80_SIGN(f) ? -1.0 / 0.0 : 1.0 / 0.0;
    if (fp80_is_nan(f))
        return 0.0 / 0.0;
    int sign = FP80_SIGN(f);
    int exp = FP80_EXP(f) - 16383;
    double mant = (double)f.mantissa / (double)(1ULL << 63);
    double result = mant;
    // Apply exponent via repeated multiply (safe for reasonable range)
    if (exp > 0) {
        for (int i = 0; i < exp && i < 1023; i++)
            result *= 2.0;
    } else if (exp < 0) {
        for (int i = 0; i > exp && i > -1023; i--)
            result /= 2.0;
    }
    return sign ? -result : result;
}

static uint64_t cmd_fpregs(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    cpu_t *cpu = system_cpu();
    if (!cpu) {
        printf("CPU not available\n");
        return 0;
    }
    fpu_state_t *fpu = (fpu_state_t *)cpu->fpu;
    if (!fpu) {
        printf("FPU not available\n");
        return 0;
    }

    for (int i = 0; i < 8; i++) {
        float80_reg_t fp = fpu->fp[i];
        double approx = fp80_to_double(fp);
        printf("FP%d = $%04X.%016llX  (%g)\n", i, fp.exponent, (unsigned long long)fp.mantissa, approx);
    }

    // FPCR decode: rounding mode and precision
    const char *rmode[] = {"RN", "RZ", "RM", "RP"};
    const char *rprec[] = {"Extended", "Single", "Double", "(reserved)"};
    uint32_t fpcr = fpu->fpcr;
    printf("FPCR  = $%08X  (%s, %s)\n", fpcr, rmode[(fpcr >> 4) & 3], rprec[(fpcr >> 6) & 3]);

    // FPSR decode: condition codes and accrued exceptions
    uint32_t fpsr = fpu->fpsr;
    printf("FPSR  = $%08X  [%c%c%c%c] Accrued:[%c%c%c%c%c]\n", fpsr, (fpsr & FPCC_N) ? 'N' : '-',
           (fpsr & FPCC_Z) ? 'Z' : '-', (fpsr & FPCC_I) ? 'I' : '-', (fpsr & FPCC_NAN) ? 'A' : '-',
           (fpsr & FPACC_IOP) ? 'I' : '-', (fpsr & FPACC_OVFL) ? 'O' : '-', (fpsr & FPACC_UNFL) ? 'U' : '-',
           (fpsr & FPACC_DZ) ? 'D' : '-', (fpsr & FPACC_INEX) ? 'X' : '-');

    printf("FPIAR = $%08X\n", fpu->fpiar);

    return 0;
}

// ============================================================================
// Configurable status line / prompt (IMP-308)
// ============================================================================

static int g_prompt_enabled = 1;

// Check if prompt/status line is enabled
int debug_prompt_enabled(void) {
    return g_prompt_enabled;
}

static uint64_t cmd_prompt(int argc, char *argv[]) {
    if (argc < 2) {
        printf("prompt is %s\n", g_prompt_enabled ? "on" : "off");
        return g_prompt_enabled;
    }
    if (strcmp(argv[1], "on") == 0) {
        g_prompt_enabled = 1;
        printf("prompt on\n");
    } else if (strcmp(argv[1], "off") == 0) {
        g_prompt_enabled = 0;
        printf("prompt off\n");
    } else {
        printf("Usage: prompt [on|off]\n");
    }
    return 0;
}

// ============================================================================
// Temporary breakpoints (IMP-601)
// ============================================================================

// Temporary breakpoint list (auto-deleted on hit)
typedef struct tbreak_node {
    uint32_t addr;
    addr_space_t space;
    struct tbreak_node *next;
} tbreak_node_t;

static tbreak_node_t *tbreak_list = NULL;

// Set a temporary breakpoint (registers a real breakpoint + tracks it)
static breakpoint_t *set_temporary_breakpoint(debug_t *debug, uint32_t addr, addr_space_t space) {
    breakpoint_t *bp = set_breakpoint(debug, addr, space);
    if (!bp)
        return NULL;
    tbreak_node_t *node = malloc(sizeof(tbreak_node_t));
    if (!node)
        return bp;
    node->addr = addr;
    node->space = space;
    node->next = tbreak_list;
    tbreak_list = node;
    return bp;
}

// Check if a temporary breakpoint was hit. If so, remove it.
void debug_check_tbreak(debug_t *debug, uint32_t pc) {
    tbreak_node_t **pp = &tbreak_list;
    while (*pp) {
        tbreak_node_t *node = *pp;
        if (node->addr == pc) {
            delete_breakpoint(debug, node->addr, node->space);
            *pp = node->next;
            free(node);
            return;
        }
        pp = &node->next;
    }
}

// ============================================================================
// Logpoint management (IMP-604)
// ============================================================================

// List all logpoints
static void list_logpoints(debug_t *debug) {
    if (!debug || !debug->logpoints) {
        printf("No logpoints set\n");
        return;
    }
    int count = 0;
    for (logpoint_t *lp = debug->logpoints; lp; lp = lp->next) {
        count++;
        if (lp->end_addr != lp->addr) {
            printf("  $%08X-$%08X", lp->addr, lp->end_addr);
        } else {
            printf("  $%08X", lp->addr);
        }
        if (lp->message)
            printf("  \"%s\"", lp->message);
        printf("  (hits: %u)\n", lp->hit_count);
    }
    printf("%d logpoint(s)\n", count);
}

// Delete a logpoint at a specific address
static int delete_logpoint(debug_t *debug, uint32_t addr) {
    logpoint_t **pp = &debug->logpoints;
    while (*pp) {
        if ((*pp)->addr == addr) {
            logpoint_t *lp = *pp;
            *pp = lp->next;
            if (lp->message)
                free(lp->message);
            free(lp);
            return 0;
        }
        pp = &(*pp)->next;
    }
    return -1; // not found
}

// Delete all logpoints
static void delete_all_logpoints(debug_t *debug) {
    logpoint_t *lp = debug->logpoints;
    while (lp) {
        logpoint_t *next = lp->next;
        if (lp->message)
            free(lp->message);
        free(lp);
        lp = next;
    }
    debug->logpoints = NULL;
}

// ============================================================================
// run-until command (IMP-504)
// ============================================================================

static uint64_t cmd_run_until(int argc, char *argv[]) {
    if (argc < 4) {
        printf("Usage: run-until <address>.b|.w|.l <op> <value> [timeout=N]\n");
        printf("  op: == != < > <= >=\n");
        printf("  Example: run-until $172.b == $80\n");
        return 0;
    }

    // Parse address with size specifier
    char target[64];
    str_to_upper(target, argv[1]);
    char *size_spec = strchr(target, '.');
    int size = 1; // default byte
    if (size_spec) {
        *size_spec = '\0';
        size_spec++;
        if (strcmp(size_spec, "W") == 0)
            size = 2;
        else if (strcmp(size_spec, "L") == 0)
            size = 4;
    }

    uint32_t addr;
    addr_space_t space;
    if (!parse_address(target, &addr, &space)) {
        printf("Invalid address: %s\n", argv[1]);
        return 0;
    }

    // Parse operator
    const char *op = argv[2];
    int op_type = -1; // 0:== 1:!= 2:< 3:> 4:<= 5:>=
    if (strcmp(op, "==") == 0)
        op_type = 0;
    else if (strcmp(op, "!=") == 0)
        op_type = 1;
    else if (strcmp(op, "<") == 0)
        op_type = 2;
    else if (strcmp(op, ">") == 0)
        op_type = 3;
    else if (strcmp(op, "<=") == 0)
        op_type = 4;
    else if (strcmp(op, ">=") == 0)
        op_type = 5;
    else {
        printf("Invalid operator: %s\n", op);
        return 0;
    }

    // Parse expected value
    const char *val_str = argv[3];
    if (*val_str == '$')
        val_str++;
    uint32_t expected = (uint32_t)strtoul(val_str, NULL, 16);

    // Parse optional timeout
    uint64_t timeout = 100000000; // default 100M instructions
    for (int i = 4; i < argc; i++) {
        if (strncmp(argv[i], "timeout=", 8) == 0) {
            timeout = strtoull(argv[i] + 8, NULL, 10);
        }
    }

    // Run scheduler in chunks, checking condition
    scheduler_t *sched = system_scheduler();
    if (!sched) {
        printf("Scheduler not available\n");
        return 0;
    }

    uint64_t start = cpu_instr_count();
    while (cpu_instr_count() - start < timeout) {
        scheduler_run_instructions(sched, 1000);
        scheduler_stop(sched);

        // Read current value
        uint32_t current;
        if (size == 1)
            current = memory_read_uint8(addr);
        else if (size == 2)
            current = memory_read_uint16(addr);
        else
            current = memory_read_uint32(addr);

        // Check condition
        int met = 0;
        switch (op_type) {
        case 0:
            met = (current == expected);
            break;
        case 1:
            met = (current != expected);
            break;
        case 2:
            met = (current < expected);
            break;
        case 3:
            met = (current > expected);
            break;
        case 4:
            met = (current <= expected);
            break;
        case 5:
            met = (current >= expected);
            break;
        }
        if (met) {
            printf("Condition met: [$%08X] = $%X after %llu instructions\n", addr, current,
                   (unsigned long long)(cpu_instr_count() - start));
            return 0;
        }
    }

    printf("Timeout after %llu instructions\n", (unsigned long long)timeout);
    return 1;
}

// ============================================================================
// Mac state summary (IMP-702)
// ============================================================================

static uint64_t cmd_mac_state(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    if (!system_memory()) {
        printf("System not initialized\n");
        return 0;
    }

    // Mouse state
    uint32_t addr_MTemp = debug_mac_lookup_global_address("MTemp");
    uint32_t addr_RawMouse = debug_mac_lookup_global_address("RawMouse");
    uint32_t addr_Mouse = debug_mac_lookup_global_address("Mouse");
    uint32_t addr_MBState = debug_mac_lookup_global_address("MBState");
    uint32_t addr_CrsrNew = debug_mac_lookup_global_address("CrsrNew");
    uint32_t addr_CrsrCouple = debug_mac_lookup_global_address("CrsrCouple");
    uint32_t addr_Ticks = debug_mac_lookup_global_address("Ticks");

    if (addr_Mouse) {
        int16_t mv = (int16_t)memory_read_uint16(addr_Mouse);
        int16_t mh = (int16_t)memory_read_uint16(addr_Mouse + 2);
        printf("Mouse:   pos=(%d,%d)", mv, mh);
    }
    if (addr_MBState) {
        uint8_t mb = memory_read_uint8(addr_MBState);
        printf("  button=%s  MBState=$%02X", (mb & 0x80) ? "UP" : "DOWN", mb);
    }
    printf("\n");

    if (addr_MTemp) {
        int16_t tv = (int16_t)memory_read_uint16(addr_MTemp);
        int16_t th = (int16_t)memory_read_uint16(addr_MTemp + 2);
        printf("Cursor:  MTemp=(%d,%d)", tv, th);
    }
    if (addr_CrsrNew) {
        printf("  CrsrNew=$%02X", memory_read_uint8(addr_CrsrNew));
    }
    if (addr_CrsrCouple) {
        printf("  CrsrCouple=$%02X", memory_read_uint8(addr_CrsrCouple));
    }
    printf("\n");

    if (addr_Ticks) {
        printf("Ticks:   %u\n", memory_read_uint32(addr_Ticks));
    }

    return 0;
}

// ============================================================================
// Lifecycle: Constructor
// ============================================================================

// ============================================================================
// Command Handlers
// ============================================================================

// --- print (replaces get, get-global, reg read) ---
static void cmd_print_handler(struct cmd_context *ctx, struct cmd_result *res) {
    // If no arg provided, delegate to raw_argv for backward compat
    if (!ctx->args[0].present) {
        cmd_err(res, "usage: print <register|global|address.size>");
        return;
    }

    struct resolved_symbol *sym = &ctx->args[0].as_sym;

    if (sym->kind == SYM_REGISTER) {
        // Special handling for FP data registers
        cpu_t *cpu = system_cpu();
        if (cpu && cpu->fpu && sym->size == 10) {
            fpu_state_t *fpu = (fpu_state_t *)cpu->fpu;
            // Extract register number from name
            int reg = -1;
            if (sym->name[0] == 'F' && sym->name[1] == 'P' && sym->name[2] >= '0' && sym->name[2] <= '7')
                reg = sym->name[2] - '0';
            if (reg >= 0) {
                float80_reg_t fp = fpu->fp[reg];
                cmd_printf(ctx, "%s = $%04X.%016llX\n", sym->name, fp.exponent, (unsigned long long)fp.mantissa);
                cmd_ok(res);
                return;
            }
        }
        // Special handling for SR (show decoded flags)
        if (strcmp(sym->name, "SR") == 0) {
            uint16_t sr = (uint16_t)sym->value;
            int s_bit = (sr >> 13) & 1;
            int im = (sr >> 8) & 7;
            int t_bit = (sr >> 15) & 1;
            int x = (sr & 0x0010) ? 1 : 0, n = (sr & 0x0008) ? 1 : 0;
            int z = (sr & 0x0004) ? 1 : 0, v = (sr & 0x0002) ? 1 : 0;
            int cc = (sr & 0x0001) ? 1 : 0;
            cmd_printf(ctx, "SR = $%04X  (S=%d, IM=%d, T=%d, XNZVC=%d%d%d%d%d)\n", sr, s_bit, im, t_bit, x, n, z, v,
                       cc);
            cmd_int(res, sym->value);
            return;
        }
        // CRP/SRP (64-bit MMU registers)
        if (strcmp(sym->name, "CRP") == 0 && g_mmu) {
            cmd_printf(ctx, "CRP = $%08X_%08X\n", (uint32_t)(g_mmu->crp >> 32), (uint32_t)(g_mmu->crp & 0xFFFFFFFF));
            cmd_ok(res);
            return;
        }
        if (strcmp(sym->name, "SRP") == 0 && g_mmu) {
            cmd_printf(ctx, "SRP = $%08X_%08X\n", (uint32_t)(g_mmu->srp >> 32), (uint32_t)(g_mmu->srp & 0xFFFFFFFF));
            cmd_ok(res);
            return;
        }

        cmd_printf(ctx, "%s = $%0*X\n", sym->name, sym->size * 2, sym->value);
        cmd_int(res, sym->value);
        return;
    }

    if (sym->kind == SYM_MAC_GLOBAL) {
        cmd_printf(ctx, "%s ($%06X) = $%0*X\n", sym->name, sym->address, sym->size * 2, sym->value);
        cmd_int(res, sym->value);
        return;
    }

    // Special targets that aren't symbols
    const char *target_name = ctx->raw_argc >= 2 ? ctx->raw_argv[1] : NULL;
    if (target_name && (strcasecmp(target_name, "instr") == 0 || strcasecmp(target_name, "$instr") == 0)) {
        uint64_t count = cpu_instr_count();
        cmd_printf(ctx, "Instruction count = %llu\n", (unsigned long long)count);
        cmd_int(res, (int64_t)count);
        return;
    }

    // SYM_UNKNOWN: try as address.size (already resolved by parser)
    if (sym->address != 0 || sym->size != 0) {
        cmd_printf(ctx, "[$%08X].%c = $%0*X\n", sym->address, sym->size == 1 ? 'b' : (sym->size == 2 ? 'w' : 'l'),
                   sym->size * 2, sym->value);
        cmd_int(res, sym->value);
        return;
    }

    cmd_err(res, "unknown target: %s", ctx->raw_argv[1]);
}

// --- examine ---
static void cmd_examine_handler(struct cmd_context *ctx, struct cmd_result *res) {
    uint32_t addr;
    if (ctx->args[0].present) {
        addr = ctx->args[0].as_addr;
    } else {
        cmd_err(res, "usage: examine <address> [count]");
        return;
    }

    uint32_t nbytes = 64;
    if (ctx->args[1].present)
        nbytes = (uint32_t)ctx->args[1].as_int;
    if (nbytes == 0) {
        cmd_err(res, "byte count must be > 0");
        return;
    }
    if (nbytes > 512)
        nbytes = 512;

    for (uint32_t i = 0; i < nbytes; i += 16) {
        cmd_printf(ctx, "$%08X  ", addr + i);
        for (uint32_t j = 0; j < 16; j++) {
            if (i + j < nbytes)
                cmd_printf(ctx, "%02x ", memory_read_uint8(addr + i + j));
            else
                cmd_printf(ctx, "   ");
        }
        cmd_printf(ctx, " ");
        for (uint32_t j = 0; j < 16; j++) {
            if (i + j < nbytes) {
                uint8_t byte = memory_read_uint8(addr + i + j);
                cmd_printf(ctx, "%c", (byte >= 0x20 && byte <= 0x7e) ? byte : '.');
            }
        }
        cmd_printf(ctx, "\n");
    }
    cmd_ok(res);
}

// --- disasm ---
static void cmd_disasm_handler(struct cmd_context *ctx, struct cmd_result *res) {
    cpu_t *cpu = system_cpu();
    if (!cpu) {
        cmd_err(res, "CPU not available");
        return;
    }

    uint32_t addr = ctx->args[0].present ? ctx->args[0].as_addr : cpu_get_pc(cpu);
    int count = ctx->args[1].present ? (int)ctx->args[1].as_int : 16;
    if (count <= 0)
        count = 16;

    for (int i = 0; i < count; i++) {
        char buf[100];
        int instr_len = debugger_disasm(buf, addr);
        cmd_printf(ctx, "%s\n", buf);
        addr += 2 * instr_len;
    }
    cmd_ok(res);
}

// --- continue (replaces run) ---
static void cmd_continue_handler(struct cmd_context *ctx, struct cmd_result *res) {
    scheduler_t *sched = system_scheduler();
    if (!sched) {
        cmd_err(res, "scheduler not available");
        return;
    }

    if (ctx->args[0].present) {
        // Counted execution: run exactly N instructions synchronously
        int64_t n = ctx->args[0].as_int;
        if (n <= 0) {
            cmd_err(res, "invalid instruction count");
            return;
        }
        scheduler_run_instructions(sched, (uint64_t)n);
    } else {
        // Unlimited execution: start async and return
        scheduler_start(sched);
    }
    cmd_ok(res);
}

// --- step ---
static void cmd_step_handler(struct cmd_context *ctx, struct cmd_result *res) {
    scheduler_t *sched = system_scheduler();
    if (!sched) {
        cmd_err(res, "scheduler not available");
        return;
    }

    int n = ctx->args[0].present ? (int)ctx->args[0].as_int : 1;
    if (n <= 0) {
        cmd_err(res, "invalid instruction count");
        return;
    }

    scheduler_run_instructions(sched, n);
    scheduler_stop(sched);
    cmd_ok(res);
}

// --- next (replaces so/step-over) ---
static void cmd_next_handler(struct cmd_context *ctx, struct cmd_result *res) {
    (void)ctx;
    cpu_t *cpu = system_cpu();
    if (!cpu) {
        cmd_err(res, "CPU not available");
        return;
    }

    uint32_t pc = cpu_get_pc(cpu);
    char buf[100];
    int instr_words = debugger_disasm(buf, pc);

    uint16_t opcode = memory_read_uint16(pc);
    int is_call = 0;
    if ((opcode & 0xFF00) == 0x6100)
        is_call = 1; // BSR
    if ((opcode & 0xFFC0) == 0x4E80)
        is_call = 1; // JSR

    if (is_call) {
        uint32_t return_addr = pc + 2 * instr_words;
        debug_t *debug = system_debug();
        if (debug) {
            set_temporary_breakpoint(debug, return_addr, ADDR_LOGICAL);
            scheduler_t *sched = system_scheduler();
            if (sched)
                scheduler_start(sched);
        }
    } else {
        scheduler_t *sched = system_scheduler();
        if (sched)
            scheduler_run_instructions(sched, 1);
    }
    cmd_ok(res);
}

// --- finish ---
static void cmd_finish_handler(struct cmd_context *ctx, struct cmd_result *res) {
    (void)ctx;
    cpu_t *cpu = system_cpu();
    if (!cpu) {
        cmd_err(res, "CPU not available");
        return;
    }

    uint32_t sp = cpu_get_an(cpu, 7);
    uint32_t return_addr = memory_read_uint32(sp);

    debug_t *debug = system_debug();
    if (!debug) {
        cmd_err(res, "debug not available");
        return;
    }

    cmd_printf(ctx, "Running until return to $%08X\n", return_addr);
    set_temporary_breakpoint(debug, return_addr, ADDR_LOGICAL);
    scheduler_t *sched = system_scheduler();
    if (sched)
        scheduler_start(sched);
    cmd_ok(res);
}

// --- until (replaces run-to) ---
static void cmd_until_handler(struct cmd_context *ctx, struct cmd_result *res) {
    if (!ctx->args[0].present) {
        cmd_err(res, "usage: until <address>");
        return;
    }
    uint32_t addr = ctx->args[0].as_addr;

    debug_t *debug = system_debug();
    if (!debug) {
        cmd_err(res, "debug not available");
        return;
    }

    set_temporary_breakpoint(debug, addr, ADDR_LOGICAL);
    scheduler_t *sched = system_scheduler();
    if (sched)
        scheduler_start(sched);
    cmd_ok(res);
}

// --- stop ---
static void cmd_stop_handler(struct cmd_context *ctx, struct cmd_result *res) {
    (void)ctx;
    scheduler_t *sched = system_scheduler();
    if (!sched) {
        cmd_err(res, "scheduler not available");
        return;
    }
    scheduler_stop(sched);
    cmd_ok(res);
}

// --- break (replaces br) ---
static void cmd_break_handler(struct cmd_context *ctx, struct cmd_result *res) {
    debug_t *debug = system_debug();
    if (!debug) {
        cmd_err(res, "debug not available");
        return;
    }

    const char *subcmd = ctx->subcmd;

    // Default or "set": set breakpoint at address
    if (subcmd == NULL || strcmp(subcmd, "set") == 0) {
        if (!ctx->args[0].present) {
            cmd_err(res, "usage: break <address>");
            return;
        }
        uint32_t addr = ctx->args[0].as_addr;
        set_breakpoint(debug, addr, ADDR_LOGICAL);
        cmd_printf(ctx, "Breakpoint set at $%08X.\n", addr);
        cmd_ok(res);
        return;
    }

    if (strcmp(subcmd, "del") == 0) {
        if (ctx->args[0].present) {
            // Check if the token is "all"
            if (ctx->raw_argc >= 3 && strcasecmp(ctx->raw_argv[2], "all") == 0) {
                int count = delete_all_breakpoints(debug);
                cmd_printf(ctx, "Deleted %d breakpoint(s).\n", count);
            } else {
                uint32_t addr = ctx->args[0].as_addr;
                if (delete_breakpoint(debug, addr, ADDR_LOGICAL))
                    cmd_printf(ctx, "Deleted breakpoint at $%08X.\n", addr);
                else
                    cmd_printf(ctx, "No breakpoint found at $%08X.\n", addr);
            }
        } else {
            cmd_err(res, "usage: break del <address|all>");
            return;
        }
        cmd_ok(res);
        return;
    }

    if (strcmp(subcmd, "list") == 0) {
        list_breakpoints(debug);
        cmd_ok(res);
        return;
    }

    if (strcmp(subcmd, "clear") == 0) {
        int count = delete_all_breakpoints(debug);
        cmd_printf(ctx, "Deleted %d breakpoint(s).\n", count);
        cmd_ok(res);
        return;
    }

    cmd_err(res, "unknown subcommand: %s", subcmd);
}

// --- tbreak ---
static void cmd_tbreak_handler(struct cmd_context *ctx, struct cmd_result *res) {
    if (!ctx->args[0].present) {
        cmd_err(res, "usage: tbreak <address>");
        return;
    }
    uint32_t addr = ctx->args[0].as_addr;
    debug_t *debug = system_debug();
    if (!debug) {
        cmd_err(res, "debug not available");
        return;
    }
    set_temporary_breakpoint(debug, addr, ADDR_LOGICAL);
    cmd_printf(ctx, "Temporary breakpoint at $%08X\n", addr);
    cmd_ok(res);
}

// --- logpoint ---
static void cmd_logpoint_handler(struct cmd_context *ctx, struct cmd_result *res) {
    // Handle bare invocation (no args) as list
    if (ctx->raw_argc <= 1 || (ctx->subcmd && strcmp(ctx->subcmd, "list") == 0)) {
        debug_t *debug = system_debug();
        if (debug)
            list_logpoints(debug);
        cmd_ok(res);
        return;
    }
    if (ctx->subcmd && strcmp(ctx->subcmd, "clear") == 0) {
        debug_t *debug = system_debug();
        if (debug) {
            delete_all_logpoints(debug);
            cmd_printf(ctx, "All logpoints deleted\n");
        }
        cmd_ok(res);
        return;
    }
    // Delegate to existing implementation for set/del (complex logpoint parsing)
    uint64_t r = cmd_logpoint(ctx->raw_argc, ctx->raw_argv);
    cmd_int(res, (int64_t)r);
}

// --- info (subcommand hub) ---
static void cmd_info_handler(struct cmd_context *ctx, struct cmd_result *res) {
    const char *subcmd = ctx->subcmd;
    if (!subcmd) {
        cmd_err(res, "usage: info <regs|fpregs|mmu|break|logpoint|mac|process|events|schedule>");
        return;
    }

    if (strcmp(subcmd, "regs") == 0) {
        // Delegate to existing td implementation
        cmd_td(0, NULL);
        cmd_ok(res);
        return;
    }

    if (strcmp(subcmd, "fpregs") == 0) {
        cmd_fpregs(0, NULL);
        cmd_ok(res);
        return;
    }

    if (strcmp(subcmd, "mmu") == 0) {
        if (g_mmu) {
            cmd_printf(ctx, "MMU enabled=%d\n", g_mmu->enabled);
            cmd_printf(ctx, "TC  = $%08X\n", g_mmu->tc);
            cmd_printf(ctx, "CRP = $%08X_%08X\n", (uint32_t)(g_mmu->crp >> 32), (uint32_t)(g_mmu->crp & 0xFFFFFFFF));
            cmd_printf(ctx, "SRP = $%08X_%08X\n", (uint32_t)(g_mmu->srp >> 32), (uint32_t)(g_mmu->srp & 0xFFFFFFFF));
            cmd_printf(ctx, "TT0 = $%08X\n", g_mmu->tt0);
            cmd_printf(ctx, "TT1 = $%08X\n", g_mmu->tt1);
        } else {
            cmd_printf(ctx, "MMU not present\n");
        }
        cmd_ok(res);
        return;
    }

    if (strcmp(subcmd, "break") == 0) {
        debug_t *debug = system_debug();
        if (debug)
            list_breakpoints(debug);
        cmd_ok(res);
        return;
    }

    if (strcmp(subcmd, "logpoint") == 0) {
        debug_t *debug = system_debug();
        if (debug)
            list_logpoints(debug);
        cmd_ok(res);
        return;
    }

    if (strcmp(subcmd, "mac") == 0) {
        cmd_mac_state(0, NULL);
        cmd_ok(res);
        return;
    }

    if (strcmp(subcmd, "process") == 0) {
        extern uint64_t cmd_process_info(int argc, char *argv[]);
        cmd_process_info(0, NULL);
        cmd_ok(res);
        return;
    }

    if (strcmp(subcmd, "events") == 0) {
        // Dispatch the existing events command
        shell_dispatch("events");
        cmd_ok(res);
        return;
    }

    if (strcmp(subcmd, "schedule") == 0) {
        shell_dispatch("schedule");
        cmd_ok(res);
        return;
    }

    cmd_err(res, "unknown info subcommand: %s", subcmd);
}

// ============================================================================
// Command Registration Tables
// ============================================================================

// --- print ---
static const char *print_aliases[] = {"p", NULL};
static const struct arg_spec print_args[] = {
    {"target", ARG_SYMBOL, "register, Mac global, or address.size"},
};

// --- set ---
static const struct arg_spec set_cmd_args[] = {
    {"target", ARG_STRING, "register, flag, or address.size"},
    {"value",  ARG_STRING, "value to set"                   },
};

// --- examine ---
static const char *examine_aliases[] = {"x", NULL};
static const struct arg_spec examine_args[] = {
    {"address", ARG_ADDR,               "start address"                         },
    {"count",   ARG_INT | ARG_OPTIONAL, "bytes to display (default 64, max 512)"},
};

// --- disasm ---
static const char *disasm_aliases[] = {"dis", "d", NULL};
static const struct arg_spec disasm_args[] = {
    {"address", ARG_ADDR | ARG_OPTIONAL, "start address (default: $pc)"       },
    {"count",   ARG_INT | ARG_OPTIONAL,  "number of instructions (default 16)"},
};

// --- continue ---
// Note: "run" is kept as a separate simple command in scheduler.c.
// "run" to the scheduler's cmd_run.
static const char *continue_aliases[] = {"c", NULL};
static const struct arg_spec continue_args[] = {
    {"count", ARG_INT | ARG_OPTIONAL, "max instructions to execute"},
};

// --- step ---
static const char *step_aliases[] = {"s", "si", NULL};
static const struct arg_spec step_args[] = {
    {"count", ARG_INT | ARG_OPTIONAL, "number of instructions (default 1)"},
};

// --- next ---
static const char *next_aliases[] = {"n", "ni", NULL};

// --- finish ---
static const char *finish_aliases[] = {"fin", NULL};

// --- until ---
static const char *until_aliases[] = {"u", NULL};
static const struct arg_spec until_args[] = {
    {"address", ARG_ADDR, "target address"},
};

// --- stop ---
static const char *stop_aliases[] = {"halt", NULL};

// --- advance ---
static const struct arg_spec advance_args[] = {
    {"address", ARG_STRING, "address.size"                              },
    {"op",      ARG_STRING, "comparison operator (==, !=, <, >, <=, >=)"},
    {"value",   ARG_STRING, "expected value"                            },
};

// --- break ---
static const char *break_aliases[] = {"b", "br", NULL};
static const struct arg_spec break_set_args[] = {
    {"address", ARG_ADDR, "breakpoint address"},
};
static const struct arg_spec break_del_args[] = {
    {"address", ARG_ADDR | ARG_OPTIONAL, "address (or 'all')"},
};
static const struct subcmd_spec break_subcmds[] = {
    {NULL,    NULL, break_set_args, 1, "set breakpoint at address"},
    {"set",   NULL, break_set_args, 1, "set breakpoint at address"},
    {"list",  NULL, NULL,           0, "list all breakpoints"     },
    {"del",   NULL, break_del_args, 1, "delete breakpoint(s)"     },
    {"clear", NULL, NULL,           0, "delete all breakpoints"   },
};

// --- tbreak ---
static const char *tbreak_aliases[] = {"tb", NULL};
static const struct arg_spec tbreak_args[] = {
    {"address", ARG_ADDR, "breakpoint address"},
};

// --- watch (alias for advance) ---
static const char *watch_aliases[] = {"w", NULL};

// --- logpoint ---
static const char *logpoint_aliases[] = {"lp", NULL};
static const struct arg_spec lp_set_args[] = {
    {"address", ARG_STRING,              "address or range"},
    {"message", ARG_REST | ARG_OPTIONAL, "log message"     },
};
static const struct arg_spec lp_del_args[] = {
    {"address", ARG_STRING, "address or 'all'"},
};
static const struct subcmd_spec logpoint_subcmds[] = {
    {NULL,    NULL, lp_set_args, 2, "set logpoint"        },
    {"set",   NULL, lp_set_args, 2, "set logpoint"        },
    {"del",   NULL, lp_del_args, 1, "delete logpoint(s)"  },
    {"list",  NULL, NULL,        0, "list all logpoints"  },
    {"clear", NULL, NULL,        0, "delete all logpoints"},
};

// --- info ---
static const char *info_aliases[] = {"i", NULL};
static const char *info_regs_aliases[] = {"r", NULL};
static const char *info_fpregs_aliases[] = {"fp", NULL};
static const char *info_break_aliases[] = {"b", NULL};
static const char *info_logpoint_aliases[] = {"lp", NULL};
static const char *info_process_aliases[] = {"proc", NULL};
static const struct subcmd_spec info_subcmds[] = {
    {"regs",     info_regs_aliases,     NULL, 0, "CPU register dump"       },
    {"fpregs",   info_fpregs_aliases,   NULL, 0, "FPU register dump"       },
    {"mmu",      NULL,                  NULL, 0, "MMU register dump"       },
    {"break",    info_break_aliases,    NULL, 0, "list all breakpoints"    },
    {"logpoint", info_logpoint_aliases, NULL, 0, "list all logpoints"      },
    {"mac",      NULL,                  NULL, 0, "Mac OS state summary"    },
    {"process",  info_process_aliases,  NULL, 0, "current application info"},
    {"events",   NULL,                  NULL, 0, "scheduler event queue"   },
    {"schedule", NULL,                  NULL, 0, "scheduler mode and CPI"  },
};

// --- translate ---
static const struct arg_spec translate_args[] = {
    {"address", ARG_ADDR, "logical address to translate"},
};

// --- addrmode ---
static const char *addrmode_values[] = {"auto", "expanded", "collapsed", NULL};
static const struct arg_spec addrmode_args[] = {
    {"mode", ARG_ENUM | ARG_OPTIONAL, "display mode", addrmode_values},
};

// --- prompt ---
static const char *prompt_values[] = {"on", "off", NULL};
static const struct arg_spec prompt_args[] = {
    {"state", ARG_ENUM | ARG_OPTIONAL, "on or off", prompt_values},
};

// --- screenshot ---
static const struct arg_spec screenshot_save_args[] = {
    {"filename", ARG_PATH, "output PNG filename"},
};
static const struct arg_spec screenshot_checksum_args[] = {
    {"top",    ARG_INT | ARG_OPTIONAL, "region top"   },
    {"left",   ARG_INT | ARG_OPTIONAL, "region left"  },
    {"bottom", ARG_INT | ARG_OPTIONAL, "region bottom"},
    {"right",  ARG_INT | ARG_OPTIONAL, "region right" },
};
static const struct arg_spec screenshot_match_args[] = {
    {"reference", ARG_PATH, "reference PNG file"},
};
static const struct subcmd_spec screenshot_subcmds[] = {
    {NULL,       NULL, screenshot_save_args,     1, "save screen as PNG"        },
    {"save",     NULL, screenshot_save_args,     1, "save screen as PNG"        },
    {"checksum", NULL, screenshot_checksum_args, 4, "compute screen checksum"   },
    {"match",    NULL, screenshot_match_args,    1, "compare with reference PNG"},
};

// --- trace ---
static const char *trace_action_values[] = {"start", "stop", NULL};
static const struct arg_spec trace_start_args[] = {
    {"action", ARG_STRING,              "start, stop, or show"    },
    {"file",   ARG_PATH | ARG_OPTIONAL, "output filename for show"},
};

// ============================================================================
// Lifecycle: Constructor
// ============================================================================

debug_t *debug_init(void) {
    debug_t *debug = (debug_t *)malloc(sizeof(debug_t));
    if (!debug) {
        return NULL;
    }
    // Zero-initialize all fields to match behavior when struct was embedded
    memset(debug, 0, sizeof(debug_t));

    // Command registrations

    // Execution control
    register_command(&(struct cmd_reg){
        .name = "continue",
        .aliases = continue_aliases,
        .category = "Execution",
        .synopsis = "Resume execution",
        .fn = cmd_continue_handler,
        .args = continue_args,
        .nargs = 1,
    });
    register_command(&(struct cmd_reg){
        .name = "step",
        .aliases = step_aliases,
        .category = "Execution",
        .synopsis = "Step one (or N) instructions",
        .fn = cmd_step_handler,
        .args = step_args,
        .nargs = 1,
    });
    register_command(&(struct cmd_reg){
        .name = "next",
        .aliases = next_aliases,
        .category = "Execution",
        .synopsis = "Step over subroutine call",
        .fn = cmd_next_handler,
    });
    register_command(&(struct cmd_reg){
        .name = "finish",
        .aliases = finish_aliases,
        .category = "Execution",
        .synopsis = "Run until current subroutine returns",
        .fn = cmd_finish_handler,
    });
    register_command(&(struct cmd_reg){
        .name = "until",
        .aliases = until_aliases,
        .category = "Execution",
        .synopsis = "Run until address is reached",
        .fn = cmd_until_handler,
        .args = until_args,
        .nargs = 1,
    });
    register_command(&(struct cmd_reg){
        .name = "stop",
        .aliases = stop_aliases,
        .category = "Execution",
        .synopsis = "Stop execution immediately",
        .fn = cmd_stop_handler,
    });
    register_command(&(struct cmd_reg){
        .name = "advance",
        .category = "Execution",
        .synopsis = "Run until memory condition is met",
        .simple_fn = cmd_run_until,
        .args = advance_args,
        .nargs = 3,
    });

    // Breakpoints and watchpoints
    register_command(&(struct cmd_reg){
        .name = "break",
        .aliases = break_aliases,
        .category = "Breakpoints",
        .synopsis = "Manage breakpoints (set/del/list/clear)",
        .fn = cmd_break_handler,
        .subcmds = break_subcmds,
        .n_subcmds = 5,
    });
    register_command(&(struct cmd_reg){
        .name = "tbreak",
        .aliases = tbreak_aliases,
        .category = "Breakpoints",
        .synopsis = "Set temporary breakpoint (auto-deletes on hit)",
        .fn = cmd_tbreak_handler,
        .args = tbreak_args,
        .nargs = 1,
    });
    register_command(&(struct cmd_reg){
        .name = "watch",
        .aliases = watch_aliases,
        .category = "Breakpoints",
        .synopsis = "Set memory watchpoint",
        .simple_fn = cmd_run_until,
        .args = advance_args,
        .nargs = 3,
    });
    register_command(&(struct cmd_reg){
        .name = "logpoint",
        .aliases = logpoint_aliases,
        .category = "Breakpoints",
        .synopsis = "Manage logpoints (set/del/list/clear)",
        .fn = cmd_logpoint_handler,
        .subcmds = logpoint_subcmds,
        .n_subcmds = 5,
    });

    // Inspection
    register_command(&(struct cmd_reg){
        .name = "print",
        .aliases = print_aliases,
        .category = "Inspection",
        .synopsis = "Print a register, global, or memory value",
        .fn = cmd_print_handler,
        .args = print_args,
        .nargs = 1,
    });
    register_command(&(struct cmd_reg){
        .name = "examine",
        .aliases = examine_aliases,
        .category = "Inspection",
        .synopsis = "Examine raw memory (hex + ASCII)",
        .fn = cmd_examine_handler,
        .args = examine_args,
        .nargs = 2,
    });
    register_command(&(struct cmd_reg){
        .name = "disasm",
        .aliases = disasm_aliases,
        .category = "Inspection",
        .synopsis = "Disassemble instructions",
        .fn = cmd_disasm_handler,
        .args = disasm_args,
        .nargs = 2,
    });
    register_command(&(struct cmd_reg){
        .name = "set",
        .category = "Inspection",
        .synopsis = "Set register, flag, or memory value",
        .simple_fn = cmd_set,
        .args = set_cmd_args,
        .nargs = 2,
    });
    register_command(&(struct cmd_reg){
        .name = "info",
        .aliases = info_aliases,
        .category = "Inspection",
        .synopsis = "Show state (regs, fpregs, mmu, break, mac, process, events)",
        .fn = cmd_info_handler,
        .subcmds = info_subcmds,
        .n_subcmds = 9,
    });

    // Tracing
    register_command(&(struct cmd_reg){
        .name = "trace",
        .category = "Tracing",
        .synopsis = "Control instruction tracing (start/stop/show)",
        .simple_fn = cmd_trace,
        .args = trace_start_args,
        .nargs = 2,
    });

    // Display
    register_command(&(struct cmd_reg){
        .name = "translate",
        .category = "Display",
        .synopsis = "Show MMU address translation",
        .simple_fn = cmd_translate,
        .args = translate_args,
        .nargs = 1,
    });
    register_command(&(struct cmd_reg){
        .name = "addrmode",
        .category = "Display",
        .synopsis = "Set address display format",
        .simple_fn = cmd_addrmode,
        .args = addrmode_args,
        .nargs = 1,
    });
    register_command(&(struct cmd_reg){
        .name = "prompt",
        .category = "Display",
        .synopsis = "Toggle PC disassembly after steps",
        .simple_fn = cmd_prompt,
        .args = prompt_args,
        .nargs = 1,
    });
    register_command(&(struct cmd_reg){
        .name = "screenshot",
        .category = "Display",
        .synopsis = "Save screen snapshot or compute checksum",
        .simple_fn = cmd_screenshot,
        .subcmds = screenshot_subcmds,
        .n_subcmds = 4,
    });

    debug_mac_init();
    return debug;
}

// ============================================================================
// Lifecycle: Destructor
// ============================================================================

// Free all resources allocated by debug_init
void debug_cleanup(debug_t *debug) {
    if (!debug)
        return;

    // Free all breakpoints
    breakpoint_t *bp = debug->breakpoints;
    while (bp) {
        breakpoint_t *next = bp->next;
        free(bp);
        bp = next;
    }
    debug->breakpoints = NULL;

    // Free all logpoints (including their messages)
    logpoint_t *lp = debug->logpoints;
    while (lp) {
        logpoint_t *next = lp->next;
        if (lp->message) {
            free(lp->message);
        }
        free(lp);
        lp = next;
    }
    debug->logpoints = NULL;

    // Free trace log buffer entries
    if (debug->trace_log_buffer) {
        for (uint32_t i = 0; i < debug->trace_log_buffer_size; i++) {
            if (debug->trace_log_buffer[i].text) {
                free(debug->trace_log_buffer[i].text);
            }
        }
        free(debug->trace_log_buffer);
        debug->trace_log_buffer = NULL;
    }

    // Free trace entries buffer
    if (debug->trace_entries) {
        free(debug->trace_entries);
        debug->trace_entries = NULL;
    }

    // Free trace buffer
    if (debug->trace_buffer) {
        free(debug->trace_buffer);
        debug->trace_buffer = NULL;
    }

    // Free the debug structure itself
    free(debug);
}

// ────────────────────────────────────────────────────────────────────────────
// Target instruction trace diagnostic output
// ────────────────────────────────────────────────────────────────────────────

// Print recent instruction trace for debugging
void debug_print_target_trace(void) {
    if (!system_is_initialized())
        return;
    debug_t *dbg = system_debug();
    if (!dbg || !dbg->trace_buffer) {
        return;
    }

    printf("\n=== Target 68K instruction trace (most recent last) ===\n");

    if (dbg->trace_head == dbg->trace_tail) {
        printf("(empty)\n");
        return;
    }

    int i;
    for (i = dbg->trace_tail; i != dbg->trace_head; i = (i + 1) % dbg->trace_buffer_size) {
        char buf[128];
        debugger_disasm(buf, dbg->trace_buffer[i]);
        printf("%s\n", buf);
    }
}

// ────────────────────────────────────────────────────────────────────────────
// Assertion failure handler (coordinates all diagnostic output)
// ────────────────────────────────────────────────────────────────────────────

// Main assertion failure handler - prints diagnostics and pauses execution
void gs_assert_fail(const char *expr, const char *file, int line, const char *func, const char *fmt, ...) {
    // Header
    printf("\n\n==================== ASSERT ====================\n");
    if (expr && *expr)
        printf("Assertion failed: (%s)\n", expr);
    else
        printf("Assertion failed\n");
    printf("at %s:%d in %s\n", file ? file : "<unknown>", line, func ? func : "<unknown>");

    // Optional message
    if (fmt) {
        printf("Message: ");
        va_list ap;
        va_start(ap, fmt);
        vprintf(fmt, ap);
        va_end(ap);
        printf("\n");
    }

    // Diagnostics - call each module's diagnostic function
    platform_print_host_callstack();
    debug_mac_print_target_backtrace();
    debug_mac_print_process_info_header();
    debug_print_target_trace();

    printf("================================================\n\n");

    bool paused = false;
    scheduler_t *sched = system_scheduler();
    if (sched) {
        if (scheduler_is_running(sched)) {
            scheduler_stop(sched);
            paused = true;
        }
    }

    if (paused) {
        printf("Emulation paused due to assertion; returning control to shell.\n");
    } else {
        printf("Assertion handled while scheduler idle; shell remains available.\n");
    }
    fflush(stdout);

    // Notify platform layer about assertion failure (e.g., for test integration)
    debug_t *debug = system_debug();
    if (debug && debug->assertion_callback) {
        debug->assertion_callback(expr, file, line, func);
    }
}
