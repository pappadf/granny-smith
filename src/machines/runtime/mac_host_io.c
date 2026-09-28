// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// mac_host_io.c
// Default Macintosh host-IO substrate methods — see mac_host_io.h.  These are
// the verbatim logic that formerly lived inline in the keyboard.press /
// mouse.move|click shell commands and the cfg->floppy branch of sys_fd_*, now
// reached uniformly through the substrate vtable.

#include "mac_host_io.h"

#include "debug_mac.h"
#include "floppy.h"
#include "keyboard.h" // key_event_t
#include "mouse.h" // input_mouse_mode_parse
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

// Every Mac family already spoke ADB keycodes at this boundary: the same int
// reaches the ADB transceiver or, on a Plus, keyboard.c, which converts to the
// M0110A's wire codes.  All this ever added was the name lookup, which now
// happens once above the substrate.
int mac_input_key(config_t *cfg, int adb_code, bool down) {
    (void)cfg;
    system_keyboard_update(down ? key_down : key_up, adb_code);
    return 0;
}

// Cursor mode string → debug_mac mode char ('d'/'g'/'h'/'a'); 0 if unknown.

int mac_input_mouse_move(config_t *cfg, int x, int y, const char *mode) {
    (void)cfg;
    char m = input_mouse_mode_parse(mode);
    if (!m)
        return -1; // unknown mode
    return debug_mac_set_mouse_mode((long)x, (long)y, m) < 0 ? -1 : 0;
}

int mac_input_mouse_button(config_t *cfg, bool down, const char *mode) {
    (void)cfg;
    char m = input_mouse_mode_parse(mode);
    if (!m)
        return -1; // unknown mode
    debug_mac_mouse_button_mode(down, m);
    return 0;
}
