# laserwriter.mk — the PLATEN switch and the embedded LaserWriter prelude.
#
# Included by BOTH Makefile (wasm) and Makefile.headless.  PLATEN=1 links
# EfterScript's platen library (the per-job PostScript interpreter behind
# the emulated LaserWriter, src/core/network/laserwriter_job.c); the
# default PLATEN=0 leaves the printer as a spool-only capture and needs no
# Rust toolchain.  Each including Makefile picks its own archive
# (PLATEN_LIB_NATIVE / PLATEN_LIB_WASM) and puts it AFTER the objects on
# the link line — a static archive resolves only what precedes it.
#
# The prelude (laserwriter_prelude.ps) is embedded as a C array the way the
# declaration-ROM fragments are (bin2c.py into build/), so the asset and the
# object it lands in are one dependency chain; it is generated regardless
# of PLATEN since it costs a python3 call and nothing else.

# This file is included before the including Makefile's first target;
# save and restore the default goal so the header rule doesn't hijack it.
LASERWRITER_SAVED_GOAL := $(.DEFAULT_GOAL)

PLATEN     ?= 0
# The EfterScript checkout: `cargo build -p platen --release` there
# produces target/release/libplaten.a (see docs/core/network/laserwriter_job.md).
PLATEN_DIR ?= ../efterscript

PLATEN_INCLUDE := -I$(PLATEN_DIR)/crates/efterscript-platen/include
PLATEN_LIB_NATIVE := $(PLATEN_DIR)/target/release/libplaten.a
# Built with `cargo build -p platen --release --target wasm32-unknown-emscripten`
# (needs emcc on the path); unverified here — no emcc in this container.
PLATEN_LIB_WASM := $(PLATEN_DIR)/target/wasm32-unknown-emscripten/release/libplaten.a
# What a Rust staticlib needs from the system on Linux, verbatim from
# `cargo rustc -p platen --release -- --print native-static-libs`.
PLATEN_NATIVE_LIBS := -lgcc_s -lutil -lrt -lpthread -lm -ldl -lc

ifeq ($(PLATEN),1)
PLATEN_CFLAGS := -DGS_PLATEN=1 $(PLATEN_INCLUDE)
else
PLATEN_CFLAGS :=
endif

LASERWRITER_DIR := src/core/network
LASERWRITER_OUT := build/laserwriter
LASERWRITER_PRELUDE_HEADER := $(LASERWRITER_OUT)/laserwriter_prelude.h

# The generated header laserwriter_job.c includes (-I$(LASERWRITER_OUT)).
$(LASERWRITER_PRELUDE_HEADER): $(LASERWRITER_DIR)/laserwriter_prelude.ps scripts/bin2c.py
	@mkdir -p $(LASERWRITER_OUT)
	python3 scripts/bin2c.py --out $@ --guard LASERWRITER_PRELUDE_H \
	  laserwriter_prelude_ps=$(LASERWRITER_DIR)/laserwriter_prelude.ps

# Restore the including Makefile's default goal.
.DEFAULT_GOAL := $(LASERWRITER_SAVED_GOAL)
