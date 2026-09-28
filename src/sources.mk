# SPDX-License-Identifier: MIT
# Copyright (c) pappadf
#
# The source and include rosters both builds share (Makefile, the WASM
# target; Makefile.headless, the native one).  Each Makefile sets CORE_DIR,
# MACHINES_DIR and PEELER_DIR before including this, and adds only what is
# its own: the platform directory, the LaserWriter transport it does not
# use, and its flags.

# Every .c under src/core and src/machines, found recursively, so a new
# subdirectory is built by both targets without anyone listing it.  Sorted,
# so the link order does not depend on the filesystem's directory order.
CORE_SRC := $(sort $(shell find $(CORE_DIR) $(MACHINES_DIR) -name '*.c'))

# The archive library, whose tree also holds its command-line tool and
# tests (built by src/peeler/Makefile and the peeler_corpus unit suite).
PEELER_SRC := $(PEELER_DIR)/lib/peeler.c \
              $(PEELER_DIR)/lib/appledouble.c \
              $(PEELER_DIR)/lib/err.c \
              $(PEELER_DIR)/lib/util.c \
              $(PEELER_DIR)/lib/formats/bin.c \
              $(PEELER_DIR)/lib/formats/cpt.c \
              $(PEELER_DIR)/lib/formats/hqx.c \
              $(PEELER_DIR)/lib/formats/sit.c \
              $(PEELER_DIR)/lib/formats/sit3.c \
              $(PEELER_DIR)/lib/formats/sit13.c \
              $(PEELER_DIR)/lib/formats/sit15.c

PEELER_INCLUDES := -I$(PEELER_DIR)/include -I$(PEELER_DIR)/lib

# Core and machine header directories.  Order matters in one place: card.h
# exists under both nubus/ and pci/, and nubus/ must come first.
CORE_INCLUDES := -I$(CORE_DIR) \
                 -I$(CORE_DIR)/cpu \
                 -I$(CORE_DIR)/cpu/dsp3210 \
                 -I$(CORE_DIR)/cpu/ppc \
                 -I$(CORE_DIR)/memory \
                 -I$(CORE_DIR)/peripherals \
                 -I$(CORE_DIR)/peripherals/nubus \
                 -I$(CORE_DIR)/peripherals/nubus/cards \
                 -I$(CORE_DIR)/peripherals/pci \
                 -I$(CORE_DIR)/peripherals/pci/cards \
                 -I$(CORE_DIR)/scheduler \
                 -I$(CORE_DIR)/debug \
                 -I$(CORE_DIR)/storage \
                 -I$(CORE_DIR)/network \
                 -I$(CORE_DIR)/shell \
                 -I$(CORE_DIR)/object \
                 -I$(CORE_DIR)/vfs \
                 -I$(MACHINES_DIR) \
                 -I$(MACHINES_DIR)/runtime \
                 -I$(MACHINES_DIR)/mac030 \
                 -I$(MACHINES_DIR)/glue \
                 -I$(MACHINES_DIR)/mdu \
                 -I$(MACHINES_DIR)/mcu \
                 -I$(MACHINES_DIR)/av \
                 -I$(MACHINES_DIR)/pdm \
                 -I$(MACHINES_DIR)/tnt \
                 -I$(MACHINES_DIR)/oss \
                 -I$(MACHINES_DIR)/compact \
                 -I$(MACHINES_DIR)/lisa
