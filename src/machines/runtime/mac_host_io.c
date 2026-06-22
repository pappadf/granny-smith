// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// mac_host_io.c
// Default Macintosh host-IO substrate methods — see mac_host_io.h.  These are
// the verbatim logic that formerly lived inline in the keyboard.press /
// mouse.move|click shell commands and the cfg->floppy branch of sys_fd_*, now
// reached uniformly through the substrate vtable (proposal §4.4).

#include "mac_host_io.h"

#include "debug_mac.h"
#include "floppy.h"
#include "keyboard.h" // key_event_t
#include "system.h"
#include "system_config.h"

#include <string.h>

int mac_fd_insert(config_t *cfg, int drive, image_t *disk) {
    return cfg->floppy ? floppy_insert(cfg->floppy, drive, disk) : -1;
}

bool mac_fd_present(config_t *cfg, int drive) {
    // No controller → treat the drive as occupied (matches the former default).
    return cfg->floppy ? floppy_is_inserted(cfg->floppy, drive) : true;
}

int mac_input_key(config_t *cfg, const char *key, bool down) {
    (void)cfg;
    int keycode = debug_mac_resolve_key_name(key);
    if (keycode < 0)
        return -1; // unknown key name
    system_keyboard_update(down ? key_down : key_up, keycode);
    return 0;
}

// Cursor mode string → debug_mac mode char ('d'/'g'/'h'/'a'); 0 if unknown.
static char mac_mouse_mode_char(const char *mode) {
    if (!mode || !*mode || strcmp(mode, "default") == 0)
        return 'd';
    if (strcmp(mode, "global") == 0)
        return 'g';
    if (strcmp(mode, "hw") == 0)
        return 'h';
    if (strcmp(mode, "aux") == 0)
        return 'a';
    return 0;
}

int mac_input_mouse_move(config_t *cfg, int x, int y, const char *mode) {
    (void)cfg;
    char m = mac_mouse_mode_char(mode);
    if (!m)
        return -1; // unknown mode
    return debug_mac_set_mouse_mode((long)x, (long)y, m) < 0 ? -1 : 0;
}

int mac_input_mouse_button(config_t *cfg, bool down, const char *mode) {
    (void)cfg;
    char m = mac_mouse_mode_char(mode);
    if (!m)
        return -1; // unknown mode
    debug_mac_mouse_button_mode(down, m);
    return 0;
}
