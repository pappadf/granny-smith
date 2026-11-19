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
#include "machine_profile.h" // machine_floppy_count
#include "mouse.h" // input_mouse_mode_parse
#include "system.h"
#include "system_internal.h"

#include <string.h>

// True when the configuration has a drive `drive` (the profile's
// floppy_slots, or the build's override)
static bool fd_drive_exists(config_t *cfg, int drive) {
    return drive >= 0 && drive < machine_floppy_count(cfg);
}

int mac_fd_insert(config_t *cfg, int drive, image_t *disk) {
    // A drive the machine does not have takes no disk
    if (!fd_drive_exists(cfg, drive))
        return -1;
    return cfg->floppy ? floppy_insert(cfg->floppy, drive, disk) : -1;
}

bool mac_fd_present(config_t *cfg, int drive) {
    // A drive the machine does not have holds no disk.  This used to answer
    // "occupied" so the core's drive auto-select would never pick it; the
    // auto-select is now bounded by the profile's floppy_slots (#177), so
    // the answer can be the true one.
    if (!fd_drive_exists(cfg, drive))
        return false;
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

// Cursor mode string → mouse_route_t (input_mouse_mode_parse, mouse.h).

int mac_input_mouse_move(config_t *cfg, int x, int y, const char *mode) {
    (void)cfg;
    mouse_route_t route = input_mouse_mode_parse(mode);
    if (route == MOUSE_ROUTE_INVALID)
        return -1; // unknown mode
    return debug_mac_set_mouse_mode((long)x, (long)y, route) < 0 ? -1 : 0;
}

int mac_input_mouse_button(config_t *cfg, bool down, const char *mode) {
    (void)cfg;
    mouse_route_t route = input_mouse_mode_parse(mode);
    if (route == MOUSE_ROUTE_INVALID)
        return -1; // unknown mode
    debug_mac_mouse_button_mode(down, route);
    return 0;
}
