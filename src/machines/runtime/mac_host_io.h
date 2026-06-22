// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// mac_host_io.h
// The default Macintosh host-IO substrate methods — floppy insertion and
// host-input (keyboard / mouse) injection — shared by every Mac substrate
// (plus, se30, iicx, iix, iici, iisi, iifx).  Each Mac substrate points its
// machine_substrate_t vtable slots straight at these; the Lisa implements its
// own (COPS keyboard/mouse + parallel FDC).
//
// This is the "one uniform path" of proposal §4.4: the former machine-level
// escape-hatch with a NULL-and-fallback in the shell commands (Lisa overrides,
// else an inline Mac default) is gone — every substrate now implements the
// methods, and the dispatch is an unconditional vtable call.
//
// Contract (matches the Lisa implementations): return 0 on success, <0 on
// failure (e.g. unknown key name, uninitialised memory, missing controller).

#ifndef GS_MACHINES_RUNTIME_MAC_HOST_IO_H
#define GS_MACHINES_RUNTIME_MAC_HOST_IO_H

#include <stdbool.h>

struct config;
struct image;

// Floppy: route to the machine's IWM/SWIM controller (cfg->floppy).
int mac_fd_insert(struct config *cfg, int drive, struct image *disk);
bool mac_fd_present(struct config *cfg, int drive);

// Host input: resolve + inject through the Mac keyboard / Toolbox-cursor path.
// `down` distinguishes key-press from key-release; `mode` is the cursor mode
// ("default" / "global" / "hw" / "aux").
int mac_input_key(struct config *cfg, const char *key, bool down);
int mac_input_mouse_move(struct config *cfg, int x, int y, const char *mode);
int mac_input_mouse_button(struct config *cfg, bool down, const char *mode);

#endif // GS_MACHINES_RUNTIME_MAC_HOST_IO_H
