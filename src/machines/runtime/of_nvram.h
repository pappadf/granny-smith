// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// of_nvram.h
// The 8 KB NVRAM of the Old World Open Firmware machines (TNT, the beige G3):
// its fixed map, the factory image a new machine is built with, and the
// Open Firmware configuration variables in it.
//
// The map has no partition headers.  Apple fixes it for these machines
// (Designing PCI Cards and Drivers for Power Macintosh Computers, 1999,
// Table 12-1, p. 443): 4 KB for other operating systems, 768 bytes for
// diagnostics (POST's log), 256 bytes of Mac OS parameter RAM, 1 KB of Name
// Registry properties, 2 KB of Open Firmware variables.  Only the last
// carries a validity check.  Its layout and checksum are the ones Apple's
// own kernel reads and writes on these machines (xnu, iokit/Kernel/
// IONVRAM.cpp, the "old world" variables code), and the firmware's own
// format pass on a blank store writes exactly what of_nvram_factory builds
// (measured on OF 1.0.5, 2.0f1 and 2.4).  A store built this way is
// accepted: the firmware skips its format pass.

#ifndef OF_NVRAM_H
#define OF_NVRAM_H

#include "rtc.h" // pram_defaults_t

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OF_NVRAM_SIZE    0x2000u
#define OF_NVRAM_DIAG    0x1000u // 768 bytes: POST's log
#define OF_NVRAM_XPRAM   0x1300u // 256 bytes: byte n is Mac OS XPRAM $n
#define OF_NVRAM_NR      0x1400u // 1 KB: Name Registry properties
#define OF_NVRAM_OF      0x1800u // 2 KB: Open Firmware variables
#define OF_NVRAM_OF_SIZE 0x800u

// One firmware's variable defaults, in the firmware's own slot order.
typedef struct of_nvram_of_defaults {
    uint32_t flags; // the eight booleans, bits 31..24 (auto-boot? = bit 29)
    uint32_t numbers[9]; // real-base .. selftest-#megs
    const char *strings[10]; // boot-device .. boot-command; NULL = empty
} of_nvram_of_defaults_t;

typedef struct of_nvram_defaults {
    const pram_defaults_t *pram; // the parameter RAM partition
    const of_nvram_of_defaults_t *of; // the Open Firmware partition
} of_nvram_defaults_t;

extern const of_nvram_defaults_t of_nvram_defaults_tnt; // OF 1.0.5 (both TNT ROMs)
extern const of_nvram_defaults_t of_nvram_defaults_g3; // OF 2.4 (Rev C)

// Build the factory store: everything zero (POST's log, the Name Registry
// area, which Mac OS reads as empty), the parameter RAM partition from
// d->pram, and a valid Open Firmware partition from d->of.
void of_nvram_factory(uint8_t nv[OF_NVRAM_SIZE], const of_nvram_defaults_t *d);

// The Open Firmware partition's header and checksum hold.
bool of_nvram_of_valid(const uint8_t nv[OF_NVRAM_SIZE]);

// The variable kinds a name can have.
typedef enum { OF_VAR_NONE = 0, OF_VAR_BOOL, OF_VAR_NUMBER, OF_VAR_STRING } of_var_kind_t;

// Read a variable as text, the way printenv shows it (true/false, hex
// without a prefix, the string itself).  Returns its kind, OF_VAR_NONE for
// an unknown name or an invalid partition.
of_var_kind_t of_nvram_getenv(const uint8_t nv[OF_NVRAM_SIZE], const char *name, char *buf, size_t buflen);

// Set a variable as setenv would: repack the strings, recompute the
// checksum.  Booleans take true/false, numbers a hex value (an optional 0x
// prefix is accepted).  Returns NULL on success, else a reason.
const char *of_nvram_setenv(uint8_t nv[OF_NVRAM_SIZE], const char *name, const char *value);

// The variable names in slot order, for listing; `count` gets the length.
const char *const *of_nvram_var_names(size_t *count);

#endif // OF_NVRAM_H
