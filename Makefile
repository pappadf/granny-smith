# Granny Smith — Top-level Build System
#
# Targets:
#   all (default)              Build WASM emulator (release mode)
#   debug                      Build WASM emulator (debug mode)
#   sanitize                   Build WASM emulator (sanitizer mode)
#   headless                   Build native headless CLI
#   unit-test                  Build and run all unit tests
#   integration-test           Build headless and run integration tests
#   integration-test-valgrind  Run integration tests under Valgrind
#   e2e-test                   Run Playwright end-to-end tests
#   test                       Run unit + integration tests
#   platen-module              Build the LaserWriter interpreter worker's module
#   run                        Build and serve the UI on :8080 (or the next free port)
#   clean                      Remove all build artifacts (wasm, headless,
#                              unit, integration, e2e)
#   help                       Show available targets

# -- Emscripten compiler + version check --

# make's built-in CC=cc outranks `CC ?= emcc`, and an environment CC (often
# gcc) would too, so the WASM compiler is set outright unless given on the
# command line.
ifneq ($(origin CC),command line)
CC := emcc
endif
EMSDK_REQUIRED_VERSION := 6.0.7

.DEFAULT_GOAL := all

# Checked when something is actually compiled or linked with it -- an
# order-only prerequisite of the WASM objects, the module and the platen
# module -- not at parse time, so goals that never run emcc (headless, the
# test targets, integration-test-<name>, ui2*) need no exemption list.
.PHONY: check-emcc
check-emcc:
	@command -v $(CC) >/dev/null 2>&1 || { \
		echo "emcc not found. Run scripts/setup_emsdk.sh $(EMSDK_REQUIRED_VERSION)"; exit 1; }
	@line=$$($(CC) --version 2>/dev/null | head -n1); \
	case "$$line" in *emcc*) ;; *) echo "Compiler is '$$line'. Expected emcc."; exit 1;; esac; \
	ver=$$(echo "$$line" | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -n1); \
	[ "$$ver" = "$(EMSDK_REQUIRED_VERSION)" ] || \
		echo "warning: emcc version $$ver != required $(EMSDK_REQUIRED_VERSION)"

# -- Directories --

BUILD_DIR     := build
OBJ_DIR       := $(BUILD_DIR)/wasm
WEB2_DIR      := app/web2
WEB2_DIST     := $(WEB2_DIR)/dist
CORE_DIR      := src/core
MACHINES_DIR  := src/machines
PLATFORM_DIR  := src/platform/wasm
PEELER_DIR    := src/peeler

# -- Sources --
# CORE_SRC, PEELER_SRC, CORE_INCLUDES and PEELER_INCLUDES are shared with
# Makefile.headless.

include src/sources.mk

# Platform-specific sources (WASM/Emscripten)
PLATFORM_SRC := $(wildcard $(PLATFORM_DIR)/*.c)

# The LaserWriter bridge reaches its interpreter through one transport per
# build (laserwriter_transport.h): in the browser the interpreter runs in
# its own worker behind a shared-memory ring, so the main module compiles
# the ring transport and links no platen archive; the direct transport is
# headless-only (Makefile.headless).
CORE_SRC := $(filter-out $(CORE_DIR)/network/laserwriter_transport_direct.c,$(CORE_SRC))

SRC := $(CORE_SRC) $(PLATFORM_SRC) $(PEELER_SRC)

# Object and dependency files (mirror source tree under OBJ_DIR)
OBJ := $(patsubst %.c,$(OBJ_DIR)/%.o,$(SRC))
DEP := $(OBJ:.o=.d)

OUTPUT := $(BUILD_DIR)/main.mjs

# -- GS declaration-ROM 68K fragments --
# Assembled by m68k binutils into build/vrom68k/ (shared with the
# headless build — the fragments are target-independent data).  Defines
# VROM68K_HEADER and the rules that produce it; gsvrom_data.c includes
# the generated header.

include src/core/peripherals/nubus/vrom68k/vrom68k.mk

# -- EfterScript platen (PLATEN=1) and the embedded LaserWriter prelude --
# Defines PLATEN, PLATEN_CFLAGS, PLATEN_LIB_WASM, PLATEN_WASM_LDFLAGS,
# PLATEN_VERSION, LASERWRITER_OUT and the rule for LASERWRITER_PRELUDE_HEADER.
# The browser always has the printer: PLATEN defaults to 1 here (headless
# keeps laserwriter.mk's 0 until EfterScript publishes an arm64 host
# archive).  `make PLATEN=0` still builds a spool-only printer.

PLATEN ?= 1
include src/core/network/laserwriter.mk

# -- Build mode (release | debug | sanitize) --

MODE ?= release

ifeq ($(MODE),debug)
	# -Og avoids excessive WASM locals that can exceed browser limits.
	# -DGS_DEBUG enables thread-affinity assertions (see worker_thread.h)
	# and any other invariants gated behind that flag.
	MODE_CFLAGS := -Og -g -DGS_DEBUG
else ifeq ($(MODE),sanitize)
	MODE_CFLAGS := -O1 -g -DGS_DEBUG -fsanitize=address,undefined -sSTACK_OVERFLOW_CHECK=2
else
	# Production profile: GS_ASSERT / scheduler invariant walks / libc assert
	# compiled out (perf proposal P4) — measured +4.5% native, more in wasm
	# (the invariant walks run per RAF frame-unit and per event insert).
	# Debug/sanitize builds keep them all.
	MODE_CFLAGS := -O2 -DGS_FAST -DNDEBUG
endif

# -- Include paths --

INCLUDES := $(CORE_INCLUDES) \
            -Isrc/platform \
            -I$(PLATFORM_DIR) \
            -I$(VROM68K_OUT) \
            -I$(LASERWRITER_OUT)

# -- Compile flags (source -> object) --
# -MMD -MP generates .d dependency files alongside each .o so that
# header changes trigger the correct recompilations.

CFLAGS := -MMD -MP $(MODE_CFLAGS) \
          -pthread \
          -DGS_PLATEN_VERSION=\"$(PLATEN_VERSION)\" \
          $(PEELER_INCLUDES) $(INCLUDES) $(PLATEN_CFLAGS) $(EXTRA_CFLAGS)

# With PLATEN=1 the main module compiles the printer bridge with its ring
# transport (laserwriter_transport_ring.c) and links NO platen archive: the
# emulator is a threaded build and Rust's prebuilt standard library for the
# Emscripten target has no atomics, so the interpreter runs in its own
# worker with its own non-threaded module — `platen-module` below, built
# from the release archive (PLATEN_LIB_WASM + PLATEN_WASM_LDFLAGS,
# laserwriter.mk).  The main link depends on neither the archive nor the
# header; only the module target fetches the archive.
PLATEN_LDLIBS :=
PLATEN_PREREQS :=

# -- The interpreter worker's module (platen-module) --
# A standalone, NON-threaded Emscripten module the page's platen worker
# imports on the first print job (app/web2/src/printer/platen.worker.ts):
# the released archive linked with the flags EfterScript's embedding guide
# gives, every platen_* entry of platen.h exported, an ES6 module for a Web
# Worker (and Node, so the app's vitest can drive it).  Named after the
# library version so a deploy never serves a stale module to a new page;
# the wasm Makefile passes the same version to em_main.c (GS_PLATEN_VERSION)
# and the page builds the URL from it.  Served beside main.mjs: the Vite dev
# middleware reads build/, `ui2` copies it into dist/.
PLATEN_MODULE_JS   := $(BUILD_DIR)/platen-$(PLATEN_VERSION).js
PLATEN_MODULE_WASM := $(BUILD_DIR)/platen-$(PLATEN_VERSION).wasm
PLATEN_MODULE_EXPORTS := _platen_job_new,_platen_job_feed,_platen_job_read_replies,_platen_job_read_errors,_platen_job_finish,_platen_job_pdf,_platen_job_error_name,_platen_job_offending,_platen_job_pages,_platen_job_free,_platen_last_error,_malloc,_free
PLATEN_MODULE_LDFLAGS := -O2 \
           $(PLATEN_WASM_LDFLAGS) \
           --no-entry \
           -sMODULARIZE=1 \
           -sEXPORT_ES6=1 \
           -sEXPORT_NAME=createPlatenModule \
           -sENVIRONMENT=worker,node \
           -sALLOW_MEMORY_GROWTH=1 \
           -sSTACK_SIZE=1MB \
           -sEXPORTED_FUNCTIONS=$(PLATEN_MODULE_EXPORTS) \
           -sEXPORTED_RUNTIME_METHODS=HEAPU8,HEAPU32,UTF8ToString \
           -sINCOMING_MODULE_JS_API=locateFile,print,printErr

# Objects depend on the flags they were compiled with, not only on their
# sources: a stamp named after a hash of CFLAGS (MODE, EXTRA_CFLAGS, PLATEN,
# the include list) is a prerequisite of every object, so `make debug` after
# `make` recompiles instead of linking release objects with the asserts
# compiled out, and toggling PLATEN rebuilds the tree instead of mixing
# objects compiled either way.
FLAGS_HASH  := $(shell printf '%s' '$(subst ','\'',$(CFLAGS) PLATEN=$(PLATEN))' | md5sum | cut -c1-12)
FLAGS_STAMP := $(OBJ_DIR)/flags-$(FLAGS_HASH).stamp

# -- Link flags (objects -> final binary) --

# ALLOW_MEMORY_GROWTH has no MAXIMUM_MEMORY, so the heap stays at wasm32's
# 2 GB default.  Keep it there: every JS shared-heap transport (camera, mic,
# audio out, Voodoo2, printer) turns a pointer into a word index with a signed
# `ptr >> 2`, which goes negative above 2 GB.  Raising the cap means switching
# those to `>>> 2` first.
LDFLAGS := $(MODE_CFLAGS) \
           -s MODULARIZE=1 \
           -s EXPORT_NAME="createModule" \
           -sWASMFS \
           -sFORCE_FILESYSTEM \
           -lopfs.js \
           -pthread \
           -sPROXY_TO_PTHREAD \
           -sOFFSCREENCANVAS_SUPPORT \
           -sOFFSCREEN_FRAMEBUFFER \
           -sOFFSCREENCANVASES_TO_PTHREAD='\#screen' \
           -s EXPORTED_RUNTIME_METHODS=['FS','stringToUTF8','UTF8ToString','HEAP16','HEAP32','HEAPU8','wasmMemory'] \
           -s EXPORTED_FUNCTIONS="['_main','_get_js_bridge']" \
           -sINCOMING_MODULE_JS_API=canvas,locateFile,mainScriptUrlOrBlob,onAbort,print,printErr \
           -s STACK_SIZE=5MB \
           -s ALLOW_MEMORY_GROWTH=1 \
           -s USE_WEBGL2=1 \
           $(EXTRA_CFLAGS) $(EXTRA_LDFLAGS)

# -- Phony targets --

.PHONY: all release debug sanitize run \
        headless unit-test integration-test integration-test-valgrind \
        e2e-test test clean help FORCE \
        ui2 ui2-dev ui2-test ui2-check ui2-check-dist ui2-prod-smoke ui2-e2e ui2-diag run2

# -- WASM build --

all: $(OUTPUT)
ifeq ($(PLATEN),1)
all: platen-module
endif

# The interpreter worker's module, from the fetched release archive.
platen-module: $(PLATEN_MODULE_JS)

$(PLATEN_MODULE_JS): $(PLATEN_LIB_WASM) | check-emcc
	@mkdir -p $(dir $@)
	@echo "Linking the platen module ($(PLATEN_VERSION)) with $(CC)"
	$(CC) $(PLATEN_MODULE_LDFLAGS) $< -o $@

release:
	$(MAKE) MODE=release all

debug:
	$(MAKE) MODE=debug all

sanitize:
	$(MAKE) MODE=sanitize all

# Compile each .c -> .o with automatic header dependency generation
$(OBJ_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# Force-rebuild build_id.o so __DATE__/__TIME__ stay current
FORCE:
$(OBJ_DIR)/$(CORE_DIR)/build_id.o: FORCE

# gsvrom_data.c embeds the generated fragments header.
$(OBJ_DIR)/$(CORE_DIR)/peripherals/nubus/gsvrom_data.o: $(VROM68K_HEADER)

# laserwriter_job.c embeds the generated prelude header.
$(OBJ_DIR)/$(CORE_DIR)/network/laserwriter_job.o: $(LASERWRITER_PRELUDE_HEADER)

# The flags stamp: creating it (any flag change) outdates every object.
$(FLAGS_STAMP):
	@mkdir -p $(dir $@)
	@rm -f $(OBJ_DIR)/flags-*.stamp $(OBJ_DIR)/platen-*.stamp
	@touch $@
$(OBJ): $(FLAGS_STAMP) $(PLATEN_PREREQS) | check-emcc

# Link all objects into the final WASM module
$(OUTPUT): $(OBJ) | check-emcc
	@mkdir -p $(dir $@)
	@echo "Linking ($(MODE)) with $(CC)"
	$(CC) $(LDFLAGS) $(OBJ) $(PLATEN_LDLIBS) -o $@

# Include auto-generated header dependency files
-include $(DEP)

# -- Run --
# Optional boot-media variables (paths relative to repo root):
#   ROM=path/to/rom.bin   VROM=path/to/vrom.bin
#   FD0=path/to/floppy.img  FD1=path/to/floppy2.img
#   HD0=path/to/hd.zip    HD1=...  (up to HD7)
#   SPEED=max|realtime|hardware

# Build the URL query string from media variables.
RUN_PARAMS :=
ifdef ROM
RUN_PARAMS += rom=/$(ROM)
endif
ifdef VROM
RUN_PARAMS += vrom=/$(VROM)
endif
ifdef FD0
RUN_PARAMS += fd0=/$(FD0)
endif
ifdef FD1
RUN_PARAMS += fd1=/$(FD1)
endif
ifdef HD0
RUN_PARAMS += hd0=/$(HD0)
endif
ifdef HD1
RUN_PARAMS += hd1=/$(HD1)
endif
ifdef HD2
RUN_PARAMS += hd2=/$(HD2)
endif
ifdef HD3
RUN_PARAMS += hd3=/$(HD3)
endif
ifdef HD4
RUN_PARAMS += hd4=/$(HD4)
endif
ifdef HD5
RUN_PARAMS += hd5=/$(HD5)
endif
ifdef HD6
RUN_PARAMS += hd6=/$(HD6)
endif
ifdef HD7
RUN_PARAMS += hd7=/$(HD7)
endif
ifdef SPEED
RUN_PARAMS += speed=$(SPEED)
endif

# Join params list with & to form query string.
EMPTY :=
SPACE := $(EMPTY) $(EMPTY)
RUN_QS = $(subst $(SPACE),&,$(strip $(RUN_PARAMS)))

# Enable fallback root when any media variable is specified.
RUN_SERVER_FLAGS :=
ifneq ($(strip $(RUN_PARAMS)),)
RUN_SERVER_FLAGS += --fallback-root .
endif

# `make run` serves the web2 UI (app/web2/dist/). Depends on `all` so the
# WASM build runs first; ui2 then copies the fresh build artifacts into the
# dist directory.
# The port is a preference: if 8080 is taken (another checkout's `make run`
# in another editor), the server moves to the next free one and prints the
# URL it actually took.  Override with RUN_PORT=8090.
RUN_PORT ?= 8080
run: all ui2
ifneq ($(strip $(RUN_PARAMS)),)
	python3 scripts/dev_server.py --root $(WEB2_DIST) --port $(RUN_PORT) $(RUN_SERVER_FLAGS) --default-params '$(RUN_QS)'
else
	python3 scripts/dev_server.py --root $(WEB2_DIST) --port $(RUN_PORT)
endif

# -- Headless native build --

headless:
	$(MAKE) -f Makefile.headless

# -- Test targets --

# Build and run all unit tests
unit-test:
	$(MAKE) -C tests/unit run

# Build headless and run all integration tests
integration-test:
	$(MAKE) -C tests/integration test

# Run a single integration test by name
integration-test-%:
	$(MAKE) -C tests/integration test-$*

# Run integration tests under Valgrind memcheck
integration-test-valgrind:
	$(MAKE) -C tests/integration test-valgrind

# Run the web2 (Svelte) Playwright end-to-end tests. Alias for ui2-e2e.
e2e-test: ui2-e2e

# Run unit + integration tests
test: unit-test integration-test

# -- UI (app/web2) — Svelte 5 + Vite + TypeScript --
# `make run` builds and serves this UI.

ui2:
	cd $(WEB2_DIR) && npm ci --silent && npm run build
	@# Copy the WASM build's runtime artifacts into the served dist directory:
	@# the module, the service worker and the LaserWriter interpreter.  Never
	@# the object tree ($(BUILD_DIR)/wasm): nothing loads it, and dist/ is what
	@# gets deployed.  Skipped if the WASM build hasn't run yet.
	@if [ -f $(BUILD_DIR)/main.mjs ]; then \
		cp $(BUILD_DIR)/main.mjs $(BUILD_DIR)/main.wasm $(WEB2_DIST)/ ; \
		if [ -f $(BUILD_DIR)/coi-serviceworker.js ]; then \
			cp $(BUILD_DIR)/coi-serviceworker.js $(WEB2_DIST)/ ; \
		fi ; \
		for f in $(BUILD_DIR)/platen-*.js $(BUILD_DIR)/platen-*.wasm; do \
			[ -f "$$f" ] && cp "$$f" $(WEB2_DIST)/ ; \
		done ; \
	else \
		echo "Note: $(BUILD_DIR)/main.mjs not found; run 'make' first to produce WASM artifacts" ; \
	fi

ui2-dev:
	cd $(WEB2_DIR) && npm run dev

ui2-test:
	cd $(WEB2_DIR) && npm test

ui2-check:
	cd $(WEB2_DIR) && npm run check && npm run lint

# Static validation of app/web2/dist/ produced by `make ui2` (or a
# bare `npm run build`). Catches origin-rooted URLs that 404 under
# subpath deploys (the v0.4.0–v0.4.2 deploy-blocker class) and
# verifies the COI service worker is wired up. Cheap — pure file
# inspection, no browser.
ui2-check-dist:
	@/usr/bin/env node scripts/check-dist.mjs

# Playwright smoke against the production-built bundle, served from
# a subpath WITHOUT pre-set COOP/COEP headers. Forces the bundle to
# bootstrap COI through its own service worker — the exact
# environment GitHub Pages provides. Requires Playwright + chromium
# already installed via tests/e2e/ (`cd tests/e2e && npm ci &&
# npx playwright install chromium`). The webServer block in the
# config starts scripts/prod-smoke-server.mjs automatically.
ui2-prod-smoke: ui2 ui2-check-dist
	cd tests/e2e && npx playwright test --config=playwright.prod-smoke.config.ts

# Playwright end-to-end test for the web2 (Svelte) Filesystem tab. Drives the
# real UI — descend a disk image, multi-select, copy out, delete, copy again —
# against the served dist; the webServer block builds (`make ui2`) and serves
# app/web2/dist with the COOP/COEP headers the worker needs. No machine boot;
# the only synthesised step is the HTML5 drag gesture (Playwright/CDP can't
# drive native DnD). Requires Playwright + chromium installed under tests/e2e.
ui2-e2e:
	cd tests/e2e && npx playwright test --config=playwright.web2.config.ts

# Headless diagnostic — spawns the dev server + drives Chromium via
# Playwright, captures console output / pageerror / xterm contents,
# and prints a JSON report. Useful for triaging "doesn't boot" bugs
# without an interactive browser. Depends on tests/e2e's Playwright
# install (chromium pre-fetched there).
#
# Environment knobs (forwarded to scripts/ui2-diag.mjs):
#   GL=swiftshader (default) | native | disable   — WebGL backend
#   COMMANDS='cpu.pc;rom list'                    — type these in
#   SETTLE=8000                                   — wait ms after boot
#   PORT=18181                                    — dev server port
#   HEADLESS=0                                    — show the browser
ui2-diag: ui2
	@/usr/bin/env node scripts/ui2-diag.mjs

# run2 is an alias for `run` for muscle-memory continuity. `run` itself
# now serves the new UI; this alias can be dropped in a future cleanup.
run2: run

# -- Clean (everything) --
# Removes all build artifacts: wasm, headless, unit, integration, e2e

clean:
	rm -rf $(BUILD_DIR)
	$(MAKE) -C tests/unit clean
	rm -rf tests/integration/test-results
	rm -rf tests/e2e/test-results

# -- Help --

help:
	@echo "Granny Smith Build System"
	@echo ""
	@echo "Build targets:"
	@echo "  all (default)              Build WASM emulator (release) and the platen module"
	@echo "  debug                      Build WASM emulator (debug)"
	@echo "  sanitize                   Build WASM emulator (sanitizers)"
	@echo "  platen-module              Build the LaserWriter interpreter worker's module"
	@echo "  headless                   Build native headless CLI"
	@echo "  run                        Build the UI and serve on :8080 (next free port if taken; RUN_PORT=n)"
	@echo ""
	@echo "UI targets (app/web2 — Svelte 5 + Vite + TS):"
	@echo "  ui2                        Build the Svelte UI (production)"
	@echo "  ui2-dev                    Start Vite dev server (HMR) on :5173"
	@echo "  ui2-check                  Run svelte-check + ESLint + Prettier"
	@echo "  ui2-test                   Run Vitest"
	@echo "  ui2-e2e                    Run the web2 Playwright e2e suite"
	@echo "  run2                       Alias for run (kept for muscle-memory)"
	@echo ""
	@echo "Test targets:"
	@echo "  test                       Run unit + integration tests"
	@echo "  unit-test                  Build and run all unit tests"
	@echo "  integration-test           Build headless; run integration tests"
	@echo "  integration-test-<name>    Run single integration test (e.g. se30-format-hd)"
	@echo "  integration-test-valgrind  Integration tests under Valgrind"
	@echo "  e2e-test                   Run the web2 Playwright e2e suite (alias for ui2-e2e)"
	@echo ""
	@echo "Maintenance:"
	@echo "  clean                      Remove all build artifacts"
	@echo "  help                       Show this message"
	@echo ""
	@echo "Options:"
	@echo "  MODE=release|debug|sanitize  Build mode (default: release)"
	@echo "  EXTRA_CFLAGS=...             Additional compiler flags"
	@echo "  PLATEN=0                     Leave out EfterScript's platen (PostScript to PDF"
	@echo "                               for the emulated LaserWriter; default on: the"
	@echo "                               bridge in the core, the interpreter in a worker)"
	@echo "  PLATEN_DIR=path              Take the platen archive from an EfterScript checkout"
	@echo ""
	@echo "Boot media (for 'run' target):"
	@echo "  ROM=path/to/rom.bin          ROM image"
	@echo "  FD0=path/to/floppy.img       Floppy disk image"
	@echo "  HD0=path/to/hd.zip  ...HD7   Hard disk images (zip or raw)"
	@echo "  SPEED=max|realtime|hardware   Emulation speed"
	@echo ""
	@echo "Example:"
	@echo "  make run ROM=tests/data/roms/plus-v3-4d1f8172.rom HD0=tests/data/systems/hd.zip"
