# SPDX-License-Identifier: MIT
# Copyright (c) pappadf
#
# build-rules.mk — the rules both product builds share: Makefile (wasm,
# OBJ_ROOT = build/wasm) and Makefile.headless (OBJ_ROOT = build/headless).
# Included after OBJ, CORE_DIR, FLAGS_STAMP and the generated-header
# variables (vrom68k.mk, pdgen.mk, gsdisk.mk, laserwriter.mk) are set: the
# targets below are expanded where the file is read.

# gsvrom_data.c embeds the generated fragments header (edit a vrom68k
# .s file → make rebuilds fragment, header, object).
$(OBJ_ROOT)/$(CORE_DIR)/peripherals/nubus/gsvrom_data.o: $(VROM68K_HEADER)

# The predecoded executors include the generated T1 headers.
$(call pdgen_cpu_objs,$(OBJ_ROOT)/$(CORE_DIR)): $(PDGEN_CPU_HEADERS)
$(call pdgen_ppc_objs,$(OBJ_ROOT)/$(CORE_DIR)): $(PDGEN_PPC_HEADERS)

# image_wrap.c embeds the generated GSDisk driver header.
$(OBJ_ROOT)/$(CORE_DIR)/storage/image_wrap.o: $(GSDISK_HEADER)

# laserwriter_job.c embeds the generated prelude header.
$(OBJ_ROOT)/$(CORE_DIR)/network/laserwriter_job.o: $(LASERWRITER_PRELUDE_HEADER)

# The flags stamp: creating it (any flag change) outdates every object.
$(FLAGS_STAMP):
	@mkdir -p $(dir $@)
	@rm -f $(OBJ_ROOT)/flags-*.stamp $(OBJ_ROOT)/platen-*.stamp
	@touch $@
