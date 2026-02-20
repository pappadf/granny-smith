// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// debug.c
// Debugging, breakpoints, logpoints, and trace functionality.

// ============================================================================
// Includes
// ============================================================================

#include "debug.h"

#include "common.h"
#include "cpu.h"
#include "debug_mac.h"
#include "log.h"
#include "memory.h"
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
    memory_map_t *mem = system_memory();
    if (!mem)
        return 0;
    memory_interface_t *iface = memory_map_interface(mem);
    return iface->read_uint16(mem, addr);
}

// ============================================================================
// Operations
// ============================================================================

breakpoint_t *set_breakpoint(debug_t *debug, uint32_t addr) {

    breakpoint_t *bp = malloc(sizeof(breakpoint_t));

    if (bp == NULL)
        return NULL;

    bp->addr = addr;

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

    sprintf(buf, "%08x  %04x  %-10s%-12s", (int)addr, (int)words[0], mnemonic, operands);

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
            if (bp->addr == current_pc) {
                printf("breakpoint hit at 0x%x\n", bp->addr);
                // Remember this PC to skip it next time we check
                debug->last_breakpoint_pc = current_pc;
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
                LOG_WITH(lp->category, lp->level, "logpoint 0x%x: %s", current_pc, lp->message);
            } else {
                LOG_WITH(lp->category, lp->level, "logpoint hit at 0x%x (hit count: %u)", current_pc, lp->hit_count);
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

// Delete a breakpoint at the specified address
// Returns true if breakpoint was found and deleted, false otherwise
static bool delete_breakpoint(debug_t *debug, uint32_t addr) {
    breakpoint_t *prev = NULL;
    breakpoint_t *bp = debug->breakpoints;

    while (bp != NULL) {
        if (bp->addr == addr) {
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
        printf("  %d: 0x%08x\n", count, (unsigned int)bp->addr);
        bp = bp->next;
        count++;
    }
}

// ============================================================================
// Shell Commands
// ============================================================================

static uint64_t cmd_breakpoint(int argc, char *argv[]) {
    debug_t *debug = system_debug();

    // No arguments: list all breakpoints
    if (argc == 1) {
        list_breakpoints(debug);
        return 0;
    }

    // Check for delete commands
    if (strcmp(argv[1], "del") == 0 || strcmp(argv[1], "delete") == 0) {
        if (argc < 3) {
            printf("Usage: br del <address|all>\n");
            return 0;
        }
        // Delete all breakpoints
        if (strcmp(argv[2], "all") == 0) {
            int count = delete_all_breakpoints(debug);
            printf("Deleted %d breakpoint(s).\n", count);
            return 0;
        }
        // Delete specific breakpoint by address
        char *endptr;
        uint32_t addr = (uint32_t)strtoul(argv[2], &endptr, 0);
        if (*endptr != '\0') {
            printf("Invalid address: %s\n", argv[2]);
            return 0;
        }
        if (delete_breakpoint(debug, addr)) {
            printf("Deleted breakpoint at 0x%08x.\n", (unsigned int)addr);
        } else {
            printf("No breakpoint found at 0x%08x.\n", (unsigned int)addr);
        }
        return 0;
    }

    // Clear all breakpoints (alias for "br del all")
    if (strcmp(argv[1], "clear") == 0) {
        int count = delete_all_breakpoints(debug);
        printf("Deleted %d breakpoint(s).\n", count);
        return 0;
    }

    // Otherwise, treat as setting a breakpoint at address
    if (argc != 2) {
        printf("Usage: br [address|del <address|all>|clear]\n");
        printf("  br              – list all breakpoints\n");
        printf("  br <address>    – set a breakpoint at address\n");
        printf("  br del <addr>   – delete breakpoint at address\n");
        printf("  br del all      – delete all breakpoints\n");
        printf("  br clear        – delete all breakpoints\n");
        return 0;
    }

    char *endptr;
    uint32_t addr = (uint32_t)strtoul(argv[1], &endptr, 0);
    if (*endptr != '\0') {
        printf("Invalid address: %s\n", argv[1]);
        return 0;
    }

    set_breakpoint(debug, addr);

    printf("Breakpoint set at 0x%08x.\n", (unsigned int)addr);

    return 0;
}

// Set a logpoint at an address with category and level
static uint64_t cmd_logpoint(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: logpoint <address|range> [message] [category=<name>] [level=<n>]\n");
        printf("  Address formats:\n");
        printf("    0x400000         - single address\n");
        printf("    0x400000-0x400100 - address range (start-end, inclusive)\n");
        printf("    0x400000:0x100   - address range (start:length)\n");
        printf("  Examples:\n");
        printf("    logpoint 0x400000 \"Reset vector\"\n");
        printf("    logpoint 0x400000-0x400100 \"ROM range\"\n");
        printf("    logpoint 0x400000:0x100 category=cpu level=10\n");
        printf("  Defaults: category=logpoint, level=0\n");
        return 0;
    }

    // Parse address or address range (required)
    char *endptr;
    uint32_t addr, end_addr;
    char *range_sep = strchr(argv[1], '-');
    char *length_sep = strchr(argv[1], ':');

    if (range_sep != NULL) {
        // Format: start-end (inclusive range)
        *range_sep = '\0';
        addr = (uint32_t)strtoul(argv[1], &endptr, 0);
        if (*endptr != '\0') {
            printf("Invalid start address: %s\n", argv[1]);
            return 0;
        }
        end_addr = (uint32_t)strtoul(range_sep + 1, &endptr, 0);
        if (*endptr != '\0') {
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
        addr = (uint32_t)strtoul(argv[1], &endptr, 0);
        if (*endptr != '\0') {
            printf("Invalid start address: %s\n", argv[1]);
            return 0;
        }
        uint32_t length = (uint32_t)strtoul(length_sep + 1, &endptr, 0);
        if (*endptr != '\0') {
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
        addr = (uint32_t)strtoul(argv[1], &endptr, 0);
        if (*endptr != '\0') {
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

    debug_t *debug = system_debug();

    // Set the logpoint
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
            printf("logpoint set at 0x%08x: %s (category: %s, level: %d)\n", (unsigned int)addr, message, category_name,
                   level);
        } else {
            printf("logpoint set at 0x%08x (category: %s, level: %d)\n", (unsigned int)addr, category_name, level);
        }
    } else {
        // Range logpoint
        if (message) {
            printf("logpoint set at 0x%08x-0x%08x: %s (category: %s, level: %d)\n", (unsigned int)addr,
                   (unsigned int)end_addr, message, category_name, level);
        } else {
            printf("logpoint set at 0x%08x-0x%08x (category: %s, level: %d)\n", (unsigned int)addr,
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

    printf("   D0 = %08x    A0 = %08x\n", (int)cpu_get_dn(cpu, 0), (int)cpu_get_an(cpu, 0));
    printf("   D1 = %08x    A1 = %08x\n", (int)cpu_get_dn(cpu, 1), (int)cpu_get_an(cpu, 1));
    printf("   D2 = %08x    A2 = %08x\n", (int)cpu_get_dn(cpu, 2), (int)cpu_get_an(cpu, 2));
    printf("   D3 = %08x    A3 = %08x\n", (int)cpu_get_dn(cpu, 3), (int)cpu_get_an(cpu, 3));
    printf("   D4 = %08x    A4 = %08x\n", (int)cpu_get_dn(cpu, 4), (int)cpu_get_an(cpu, 4));
    printf("   D5 = %08x    A5 = %08x\n", (int)cpu_get_dn(cpu, 5), (int)cpu_get_an(cpu, 5));
    printf("   D6 = %08x    A6 = %08x\n", (int)cpu_get_dn(cpu, 6), (int)cpu_get_an(cpu, 6));
    printf("   D7 = %08x    A7 = %08x\n", (int)cpu_get_dn(cpu, 7), (int)cpu_get_an(cpu, 7));

    return 0;
}

static uint64_t cmd_step(int argc, char *argv[]) {
    printf("cmd_step called with %d args\n", argc);

    if (argc > 2) {
        printf("Usage: s [n instructions]\n");
        return 0;
    }

    int n = 1;
    if (argc == 2) {
        n = atoi(argv[1]);
        if (n <= 0) {
            printf("Invalid number of instructions\n");
            return 0;
        }
    }

    scheduler_t *sched = system_scheduler();
    if (!sched) {
        printf("Scheduler not available\n");
        return 0;
    }

    scheduler_run_instructions(sched, n);

    // Explicitly stop the scheduler after single-stepping
    scheduler_stop(sched);

    return 0;
}

static uint64_t cmd_disasm(int argc, char *argv[]) {
    uint32_t addr;
    int n = 10; // default number of instructions

    if (argc < 2) {
        printf("Usage: disasm <address> [n instructions]\n");
        return 0;
    }

    char *endptr;
    addr = (uint32_t)strtoul(argv[1], &endptr, 0);
    if (*endptr != '\0') {
        printf("Invalid address: %s\n", argv[1]);
        return 0;
    }

    if (argc >= 3) {
        n = atoi(argv[2]);
        if (n <= 0)
            n = 10;
    }

    for (int i = 0; i < n; i++) {
        char buf[100];
        int instr_len = debugger_disasm(buf, addr);
        printf("%s\n", buf);
        addr += 2 * instr_len;
    }

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

    // Parse the value
    char *endptr;
    uint32_t value = (uint32_t)strtoul(argv[2], &endptr, 0);
    if (*endptr != '\0') {
        printf("Invalid value: %s\n", argv[2]);
        return 0;
    }

    // Data registers (D0-D7)
    if (target[0] == 'D' && target[1] >= '0' && target[1] <= '7' && target[2] == '\0') {
        int reg = target[1] - '0';
        cpu_set_dn(cpu, reg, value);
        printf("D%d = 0x%08x\n", reg, value);
        return 0;
    }

    // Address registers (A0-A7)
    if (target[0] == 'A' && target[1] >= '0' && target[1] <= '7' && target[2] == '\0') {
        int reg = target[1] - '0';
        cpu_set_an(cpu, reg, value);
        printf("A%d = 0x%08x\n", reg, value);
        return 0;
    }

    // Program Counter
    if (strcmp(target, "PC") == 0) {
        cpu_set_pc(cpu, value);
        printf("PC = 0x%08x\n", value);
        return 0;
    }

    // Stack Pointer (alias for A7)
    if (strcmp(target, "SP") == 0) {
        cpu_set_an(cpu, 7, value);
        printf("SP = 0x%08x\n", value);
        return 0;
    }

    // Supervisor Stack Pointer
    if (strcmp(target, "SSP") == 0) {
        cpu_set_ssp(cpu, value);
        printf("SSP = 0x%08x\n", value);
        return 0;
    }

    // User Stack Pointer
    if (strcmp(target, "USP") == 0) {
        cpu_set_usp(cpu, value);
        printf("USP = 0x%08x\n", value);
        return 0;
    }

    // Status Register
    if (strcmp(target, "SR") == 0) {
        cpu_set_sr(cpu, (uint16_t)value);
        printf("SR = 0x%04x\n", (uint16_t)value);
        return 0;
    }

    // Condition Code Register
    if (strcmp(target, "CCR") == 0) {
        uint16_t sr = cpu_get_sr(cpu);
        sr = (sr & ~cpu_ccr_mask) | (value & cpu_ccr_mask);
        cpu_set_sr(cpu, sr);
        printf("CCR = 0x%02x\n", (uint8_t)(value & cpu_ccr_mask));
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

    // Memory addresses with size specifiers (.b, .w, .l)
    char *size_spec = strchr(target, '.');
    if (size_spec != NULL) {
        *size_spec = '\0';
        size_spec++;

        // Parse the address from target
        uint32_t addr = (uint32_t)strtoul(target, &endptr, 0);
        if (*endptr != '\0') {
            printf("Invalid address: %s\n", target);
            return 0;
        }

        if (strcmp(size_spec, "B") == 0) {
            memory_write_uint8(addr, (uint8_t)value);
            printf("[0x%08x].b = 0x%02x\n", addr, (uint8_t)value);
            return 0;
        } else if (strcmp(size_spec, "W") == 0) {
            memory_write_uint16(addr, (uint16_t)value);
            printf("[0x%08x].w = 0x%04x\n", addr, (uint16_t)value);
            return 0;
        } else if (strcmp(size_spec, "L") == 0) {
            memory_write_uint32(addr, value);
            printf("[0x%08x].l = 0x%08x\n", addr, value);
            return 0;
        } else {
            printf("Invalid size specifier: .%s (use .b, .w, or .l)\n", size_spec);
            return 0;
        }
    }

    printf("Unknown target: %s\n", target);
    printf("Usage: set <register|address> <value>\n");
    printf("  Registers: D0-D7, A0-A7, PC, SP, SSP, USP, SR, CCR\n");
    printf("  Condition codes: C, V, Z, N, X\n");
    printf("  Memory: <address>.b|.w|.l <value>\n");
    return 0;
}

// Get command - read and return register or memory value
static uint64_t cmd_get(int argc, char *argv[]) {
    if (argc != 2) {
        printf("Usage: get <register|address|instr>\n");
        printf("  Registers: D0-D7, A0-A7, PC, SP, SSP, USP, SR, CCR\n");
        printf("  Condition codes: C, V, Z, N, X\n");
        printf("  Memory: <address>.b|.w|.l\n");
        printf("  Instruction count: instr\n");
        return 0;
    }

    cpu_t *cpu = system_cpu();
    if (!cpu) {
        printf("CPU not available\n");
        return 0;
    }
    char target[64];
    str_to_upper(target, argv[1]);
    uint64_t value = 0;

    // Instruction count
    if (strcmp(target, "INSTR") == 0) {
        value = cpu_instr_count();
        printf("Instruction count = %llu\n", (unsigned long long)value);
        return value;
    }

    // Data registers (D0-D7)
    if (target[0] == 'D' && target[1] >= '0' && target[1] <= '7' && target[2] == '\0') {
        int reg = target[1] - '0';
        value = cpu_get_dn(cpu, reg);
        printf("D%d = 0x%08x\n", reg, value);
        return value;
    }

    // Address registers (A0-A7)
    if (target[0] == 'A' && target[1] >= '0' && target[1] <= '7' && target[2] == '\0') {
        int reg = target[1] - '0';
        value = cpu_get_an(cpu, reg);
        printf("A%d = 0x%08x\n", reg, value);
        return value;
    }

    // Program Counter
    if (strcmp(target, "PC") == 0) {
        value = cpu_get_pc(cpu);
        printf("PC = 0x%08x\n", value);
        return value;
    }

    // Stack Pointer (alias for A7)
    if (strcmp(target, "SP") == 0) {
        value = cpu_get_an(cpu, 7);
        printf("SP = 0x%08x\n", value);
        return value;
    }

    // Supervisor Stack Pointer
    if (strcmp(target, "SSP") == 0) {
        value = cpu_get_ssp(cpu);
        printf("SSP = 0x%08x\n", value);
        return value;
    }

    // User Stack Pointer
    if (strcmp(target, "USP") == 0) {
        value = cpu_get_usp(cpu);
        printf("USP = 0x%08x\n", value);
        return value;
    }

    // Status Register
    if (strcmp(target, "SR") == 0) {
        value = cpu_get_sr(cpu);
        printf("SR = 0x%04x\n", (uint16_t)value);
        return value;
    }

    // Condition Code Register
    if (strcmp(target, "CCR") == 0) {
        uint16_t sr = cpu_get_sr(cpu);
        value = sr & cpu_ccr_mask;
        printf("CCR = 0x%02x\n", (uint8_t)value);
        return value;
    }

    // Individual condition code flags
    if (strcmp(target, "C") == 0) {
        uint16_t sr = cpu_get_sr(cpu);
        value = (sr & cpu_ccr_c) ? 1 : 0;
        printf("C = %d\n", value);
        return value;
    }

    if (strcmp(target, "V") == 0) {
        uint16_t sr = cpu_get_sr(cpu);
        value = (sr & cpu_ccr_v) ? 1 : 0;
        printf("V = %d\n", value);
        return value;
    }

    if (strcmp(target, "Z") == 0) {
        uint16_t sr = cpu_get_sr(cpu);
        value = (sr & cpu_ccr_z) ? 1 : 0;
        printf("Z = %d\n", value);
        return value;
    }

    if (strcmp(target, "N") == 0) {
        uint16_t sr = cpu_get_sr(cpu);
        value = (sr & cpu_ccr_n) ? 1 : 0;
        printf("N = %d\n", value);
        return value;
    }

    if (strcmp(target, "X") == 0) {
        uint16_t sr = cpu_get_sr(cpu);
        value = (sr & cpu_ccr_x) ? 1 : 0;
        printf("X = %d\n", value);
        return value;
    }

    // Memory addresses with size specifiers (.b, .w, .l)
    char *size_spec = strchr(target, '.');
    if (size_spec != NULL) {
        *size_spec = '\0';
        size_spec++;

        // Parse the address from target
        char *endptr;
        uint32_t addr = (uint32_t)strtoul(target, &endptr, 0);
        if (*endptr != '\0') {
            printf("Invalid address: %s\n", target);
            return 0;
        }

        if (strcmp(size_spec, "B") == 0) {
            value = memory_read_uint8(addr);
            printf("[0x%08x].b = 0x%02x\n", addr, (uint8_t)value);
            return value;
        } else if (strcmp(size_spec, "W") == 0) {
            value = memory_read_uint16(addr);
            printf("[0x%08x].w = 0x%04x\n", addr, (uint16_t)value);
            return value;
        } else if (strcmp(size_spec, "L") == 0) {
            value = memory_read_uint32(addr);
            printf("[0x%08x].l = 0x%08x\n", addr, value);
            return value;
        } else {
            printf("Invalid size specifier: .%s (use .b, .w, or .l)\n", size_spec);
            return 0;
        }
    }

    printf("Unknown target: %s\n", target);
    printf("Usage: get <register|address>\n");
    printf("  Registers: D0-D7, A0-A7, PC, SP, SSP, USP, SR, CCR\n");
    printf("  Condition codes: C, V, Z, N, X\n");
    printf("  Memory: <address>.b|.w|.l\n");
    return 0;
}

// Examine command - display raw memory in hex and ASCII
static uint64_t cmd_examine(int argc, char *argv[]) {
    if (argc < 2 || argc > 3) {
        printf("Usage: x <address> [n]\n");
        printf("  Display memory at address in hex and ASCII format\n");
        printf("  n = number of bytes to display (default: 64, max: 512)\n");
        return 0;
    }

    // Parse the address
    char *endptr;
    uint32_t addr = (uint32_t)strtoul(argv[1], &endptr, 0);
    if (*endptr != '\0') {
        printf("Invalid address: %s\n", argv[1]);
        return 0;
    }

    // Parse the number of bytes (default: 64, max: 512)
    uint32_t nbytes = 64;
    if (argc == 3) {
        nbytes = (uint32_t)strtoul(argv[2], &endptr, 0);
        if (*endptr != '\0') {
            printf("Invalid byte count: %s\n", argv[2]);
            return 0;
        }
        if (nbytes == 0) {
            printf("Byte count must be greater than 0\n");
            return 0;
        }
        if (nbytes > 512) {
            printf("Byte count exceeds maximum (512)\n");
            nbytes = 512;
        }
    }

    // Display memory in 16-byte rows
    for (uint32_t i = 0; i < nbytes; i += 16) {
        // Print address
        printf("%08x  ", addr + i);

        // Print hex bytes
        for (uint32_t j = 0; j < 16; j++) {
            if (i + j < nbytes) {
                uint8_t byte = memory_read_uint8(addr + i + j);
                printf("%02x ", byte);
            } else {
                printf("   ");
            }
        }

        // Print ASCII representation
        printf(" ");
        for (uint32_t j = 0; j < 16; j++) {
            if (i + j < nbytes) {
                uint8_t byte = memory_read_uint8(addr + i + j);
                // Print printable ASCII characters, otherwise print '.'
                if (byte >= 0x20 && byte <= 0x7e) {
                    printf("%c", byte);
                } else {
                    printf(".");
                }
            }
        }
        printf("\n");
    }

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

    // Register commands
    register_cmd("s", "Debugger",
                 "s [n instructions]  – step through the next, or through a specified number of instructions",
                 &cmd_step);
    register_cmd("br", "Debugger", "br [address|del <address|all>|clear]  – manage breakpoints", &cmd_breakpoint);
    register_cmd("logpoint", "Debugger",
                 "logpoint <address|range> [message] [category=<name>] [level=<n>]  – set a logpoint (supports addr, "
                 "addr-end, addr:len)",
                 &cmd_logpoint);
    register_cmd("td", "Debugger", "td  – display CPU registers", &cmd_td);
    register_cmd(
        "trace", "Debugger",
        "trace <start|stop|show [filename]>  – control instruction tracing (save to file when filename provided)",
        &cmd_trace);
    register_cmd("disasm", "Debugger", "disasm <address> [n]  – disassemble n instructions at address", &cmd_disasm);
    register_cmd("set", "Debugger", "set <register|address> <value>  – set register, condition code, or memory value",
                 &cmd_set);
    register_cmd("get", "Debugger", "get <register|address>  – get register, condition code, or memory value",
                 &cmd_get);
    register_cmd("x", "Debugger", "x <address> [n]  – examine raw memory in hex and ASCII format", &cmd_examine);
    register_cmd("screenshot", "Debugger", "screenshot <filename.png>  – save the emulated screen as a PNG file",
                 &cmd_screenshot);

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
