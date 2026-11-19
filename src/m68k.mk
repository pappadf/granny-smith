# SPDX-License-Identifier: MIT
# Copyright (c) pappadf
#
# m68k.mk — the one recipe that turns 68k assembly into a flat binary,
# shared by vrom68k.mk (the NuBus declaration-ROM fragments) and gsdisk.mk
# (the GSDisk driver).  Needs m68k binutils (binutils-m68k-linux-gnu, or any
# m68k-targeted pair named by M68K_AS / M68K_OBJCOPY), with no fallback.

M68K_AS      ?= m68k-linux-gnu-as
M68K_OBJCOPY ?= m68k-linux-gnu-objcopy

# The recipe body, for a target `$@`: $(1) is the assembler's arguments
# (flags and source), $(2) the object's path without its .o.  Assemble,
# then flatten (objcopy -O binary is safe only with no relocations: every
# reference must be PC-relative or a same-section difference).  The .o and
# .bin are written under temporary names and renamed, so a build
# interrupted, or racing another target's, never leaves a torn binary that
# make then trusts as up to date.
define m68k_flat_bin
	@command -v $(M68K_AS) >/dev/null 2>&1 || { \
	  echo "error: $(M68K_AS) not found — install binutils-m68k-linux-gnu" >&2; \
	  echo "       (or point M68K_AS/M68K_OBJCOPY at another m68k binutils)" >&2; \
	  exit 1; }
	@mkdir -p $(dir $@)
	$(M68K_AS) $(1) -o $(2).o.$$$$ && \
	$(M68K_OBJCOPY) -O binary $(2).o.$$$$ $@.$$$$ && \
	mv -f $(2).o.$$$$ $(2).o && mv -f $@.$$$$ $@
endef
