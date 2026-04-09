// Shell command stubs for unit tests
// Provides no-op implementations of shell command registration and dispatch.

#include "cmd_types.h"

int register_command(const struct cmd_reg *reg) {
    (void)reg;
    return 0;
}

int register_cmd(const char *name, const char *category, const char *synopsis, cmd_fn_simple fn) {
    (void)name;
    (void)category;
    (void)synopsis;
    (void)fn;
    return 0;
}

int unregister_cmd(const char *name) {
    (void)name;
    return 0;
}

int shell_init(void) {
    return 0;
}

uint64_t shell_dispatch(char *line) {
    (void)line;
    return 0;
}
