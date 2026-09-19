# laserwriter.mk — the PLATEN switch, the platen library fetch, and the
# embedded LaserWriter prelude.
#
# Included by BOTH Makefile (wasm) and Makefile.headless.  PLATEN=1 builds
# the printer bridge (src/core/network/laserwriter_job.c: the per-job
# PostScript interpreter behind the emulated LaserWriter); the default
# PLATEN=0 leaves the printer as a spool-only capture.  The bridge reaches
# EfterScript's platen library through one transport per build
# (laserwriter_transport.h): headless links the host archive and calls it
# directly (laserwriter_transport_direct.c); the browser build compiles
# the ring transport (laserwriter_transport_ring.c) and links NO archive —
# the interpreter runs in a Web Worker with its own non-threaded module,
# which part 2B builds from the Emscripten archive named below.
#
# The library comes prebuilt from EfterScript's releases, pinned by
# PLATEN_VERSION: every tag attaches a host archive, an Emscripten archive
# named after the SDK it was built with, the header, and SHA256SUMS.
# scripts/fetch_platen.sh fetches and verifies them into PLATEN_CACHE, so
# PLATEN=1 needs no Rust toolchain.  Makefile.headless puts PLATEN_LIB_NATIVE
# AFTER the objects on the link line — a static archive resolves only what
# precedes it.  The Emscripten archive (PLATEN_LIB_WASM) links only with
# the SDK version in its name (Makefile pins EMSDK_REQUIRED_VERSION to the
# same) and only with WebAssembly exceptions enabled at the link
# (PLATEN_WASM_LDFLAGS).
#
# A developer working on EfterScript itself can point PLATEN_DIR at a
# checkout: its `cargo build -p platen --release [--target
# wasm32-unknown-emscripten]` outputs are used instead of the release.
#
# The prelude (laserwriter_prelude.ps) is embedded as a C array the way the
# declaration-ROM fragments are (bin2c.py into build/), so the asset and the
# object it lands in are one dependency chain; it is generated regardless
# of PLATEN since it costs a python3 call and nothing else.

# This file is included before the including Makefile's first target;
# save and restore the default goal so the rules here don't hijack it.
LASERWRITER_SAVED_GOAL := $(.DEFAULT_GOAL)

PLATEN         ?= 0
# The EfterScript release the library comes from (tag v$(PLATEN_VERSION)).
PLATEN_VERSION ?= 0.0.2
# The Emscripten SDK the release's wasm archive was built with; must equal
# the SDK this tree builds with (EMSDK_REQUIRED_VERSION in Makefile).
PLATEN_EMSDK   ?= $(if $(EMSDK_REQUIRED_VERSION),$(EMSDK_REQUIRED_VERSION),6.0.7)
# Where fetched releases are kept (local/ is ignored by git, survives clean).
PLATEN_CACHE   ?= local/platen/$(PLATEN_VERSION)
# Optional: an EfterScript checkout whose cargo outputs replace the release.
PLATEN_DIR     ?=

# The host archive's target triple, from the machine running the build.
PLATEN_HOST_ARCH := $(shell uname -m)
PLATEN_HOST_OS   := $(shell uname -s)
ifeq ($(PLATEN_HOST_OS),Linux)
PLATEN_HOST_TRIPLE := $(PLATEN_HOST_ARCH)-unknown-linux-gnu
else ifeq ($(PLATEN_HOST_OS),Darwin)
PLATEN_HOST_TRIPLE := $(if $(filter arm64,$(PLATEN_HOST_ARCH)),aarch64,$(PLATEN_HOST_ARCH))-apple-darwin
else
PLATEN_HOST_TRIPLE := $(PLATEN_HOST_ARCH)-unknown-$(PLATEN_HOST_OS)
endif

PLATEN_ASSET_NATIVE := libplaten-$(PLATEN_VERSION)-$(PLATEN_HOST_TRIPLE).a
PLATEN_ASSET_WASM   := libplaten-$(PLATEN_VERSION)-wasm32-unknown-emscripten-$(PLATEN_EMSDK).a
PLATEN_ASSET_HEADER := platen-$(PLATEN_VERSION).h

ifneq ($(PLATEN_DIR),)
# Developer override: EfterScript's own build outputs.
PLATEN_INCLUDE    := -I$(PLATEN_DIR)/crates/efterscript-platen/include
PLATEN_LIB_NATIVE := $(PLATEN_DIR)/target/release/libplaten.a
PLATEN_LIB_WASM   := $(PLATEN_DIR)/target/wasm32-unknown-emscripten/release/libplaten.a
PLATEN_HEADER     :=
else
# The release: the header is copied to platen.h beside the archives so
# `#include "platen.h"` resolves as it does against a checkout.
PLATEN_INCLUDE    := -I$(PLATEN_CACHE)
PLATEN_LIB_NATIVE := $(PLATEN_CACHE)/$(PLATEN_ASSET_NATIVE)
PLATEN_LIB_WASM   := $(PLATEN_CACHE)/$(PLATEN_ASSET_WASM)
PLATEN_HEADER     := $(PLATEN_CACHE)/platen.h

$(PLATEN_CACHE)/$(PLATEN_ASSET_HEADER): scripts/fetch_platen.sh
	scripts/fetch_platen.sh $(PLATEN_VERSION) $(PLATEN_CACHE) $(PLATEN_ASSET_HEADER)

$(PLATEN_HEADER): $(PLATEN_CACHE)/$(PLATEN_ASSET_HEADER)
	cp -f $< $@

$(PLATEN_LIB_NATIVE): scripts/fetch_platen.sh
	scripts/fetch_platen.sh $(PLATEN_VERSION) $(PLATEN_CACHE) $(PLATEN_ASSET_NATIVE)

$(PLATEN_LIB_WASM): scripts/fetch_platen.sh
	scripts/fetch_platen.sh $(PLATEN_VERSION) $(PLATEN_CACHE) $(PLATEN_ASSET_WASM)
endif

# What a Rust staticlib needs from the system on Linux, verbatim from
# `cargo rustc -p platen --release -- --print native-static-libs`.
PLATEN_NATIVE_LIBS := -lgcc_s -lutil -lrt -lpthread -lm -ldl -lc
# Rust compiles the Emscripten target with WebAssembly exceptions on (the
# legacy form); the final link must enable them or the archive's exception
# tag is undefined (EfterScript's embedding guide).
PLATEN_WASM_LDFLAGS := -fwasm-exceptions -sWASM_LEGACY_EXCEPTIONS=1

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
