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
#   run                        Build and start HTTP server on :8080
#   clean                      Remove all build artifacts (wasm, headless,
#                              unit, integration, e2e)
#   help                       Show available targets

# -- Emscripten compiler + version guard --

CC ?= emcc
ifneq ($(notdir $(CC)),emcc)
	ifneq (,$(shell command -v emcc 2>/dev/null))
		override CC := emcc
	endif
endif
EMSDK_REQUIRED_VERSION := 4.0.10

# Targets that do not require the Emscripten toolchain
NON_EMCC_TARGETS := clean help headless unit-test \
                    integration-test integration-test-valgrind e2e-test test

# Only validate emcc when a WASM build target is requested
ifeq (,$(filter $(NON_EMCC_TARGETS),$(MAKECMDGOALS)))
ifeq (,$(shell command -v $(CC) 2>/dev/null))
$(error emcc not found. Run scripts/setup_emsdk.sh $(EMSDK_REQUIRED_VERSION))
endif
EMCC_VERSION_LINE := $(shell $(CC) --version 2>/dev/null | head -n1)
EMCC_VERSION := $(shell echo "$(EMCC_VERSION_LINE)" | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -n1)
ifeq ($(strip $(EMCC_VERSION_LINE)),)
$(error Unable to execute $(CC); ensure Emscripten environment is loaded)
endif
ifeq ($(findstring emcc,$(EMCC_VERSION_LINE)),)
$(error Compiler is '$(EMCC_VERSION_LINE)'. Expected emcc.)
endif
ifneq ($(EMCC_VERSION),$(EMSDK_REQUIRED_VERSION))
$(warning emcc version $(EMCC_VERSION) != required $(EMSDK_REQUIRED_VERSION))
endif
endif

# -- Submodule guard --
# Fail early if required git submodules are not initialised.

ifeq ($(wildcard third-party/peeler/include),)
$(error Submodule third-party/peeler not initialised. Run: git submodule update --init --recursive)
endif

# -- Directories --

BUILD_DIR    := build
OBJ_DIR      := $(BUILD_DIR)/wasm
WEB_DIR      := app/web
CORE_DIR     := src/core
PLATFORM_DIR := src/platform/wasm
PEELER_DIR   := third-party/peeler

# -- Source discovery --
# Wildcard patterns auto-discover new .c files in each subdirectory.

# Core emulator sources (platform-agnostic)
CORE_SRC := $(wildcard $(CORE_DIR)/*.c) \
            $(wildcard $(CORE_DIR)/cpu/*.c) \
            $(wildcard $(CORE_DIR)/memory/*.c) \
            $(wildcard $(CORE_DIR)/peripherals/*.c) \
            $(wildcard $(CORE_DIR)/scheduler/*.c) \
            $(wildcard $(CORE_DIR)/debug/*.c) \
            $(wildcard $(CORE_DIR)/storage/*.c) \
            $(wildcard $(CORE_DIR)/network/*.c) \
            $(wildcard $(CORE_DIR)/shell/*.c)

# Platform-specific sources (WASM/Emscripten)
PLATFORM_SRC := $(wildcard $(PLATFORM_DIR)/*.c)

# Peeler library sources
PEELER_SRC := $(PEELER_DIR)/lib/peeler.c \
              $(PEELER_DIR)/lib/err.c \
              $(PEELER_DIR)/lib/util.c \
              $(PEELER_DIR)/lib/formats/bin.c \
              $(PEELER_DIR)/lib/formats/cpt.c \
              $(PEELER_DIR)/lib/formats/hqx.c \
              $(PEELER_DIR)/lib/formats/sit.c \
              $(PEELER_DIR)/lib/formats/sit13.c \
              $(PEELER_DIR)/lib/formats/sit15.c

SRC := $(CORE_SRC) $(PLATFORM_SRC) $(PEELER_SRC)

# Object and dependency files (mirror source tree under OBJ_DIR)
OBJ := $(patsubst %.c,$(OBJ_DIR)/%.o,$(SRC))
DEP := $(OBJ:.o=.d)

OUTPUT := $(BUILD_DIR)/main.mjs

# -- Build mode (release | debug | sanitize) --

MODE ?= release

ifeq ($(MODE),debug)
	# -Og avoids excessive WASM locals that can exceed browser limits
	MODE_CFLAGS := -Og -g
else ifeq ($(MODE),sanitize)
	MODE_CFLAGS := -O1 -g -fsanitize=address,undefined -sSTACK_OVERFLOW_CHECK=2
else
	MODE_CFLAGS := -O2
endif

# -- Include paths --

PEELER_INCLUDES := -I$(PEELER_DIR)/include -I$(PEELER_DIR)/lib

INCLUDES := -I$(CORE_DIR) \
            -I$(CORE_DIR)/cpu \
            -I$(CORE_DIR)/memory \
            -I$(CORE_DIR)/peripherals \
            -I$(CORE_DIR)/scheduler \
            -I$(CORE_DIR)/debug \
            -I$(CORE_DIR)/storage \
            -I$(CORE_DIR)/network \
            -I$(CORE_DIR)/shell \
            -I$(PLATFORM_DIR)

# -- Compile flags (source -> object) --
# -MMD -MP generates .d dependency files alongside each .o so that
# header changes trigger the correct recompilations.

CFLAGS := -MMD -MP $(MODE_CFLAGS) \
          $(PEELER_INCLUDES) $(INCLUDES) $(EXTRA_CFLAGS)

# -- Link flags (objects -> final binary) --

LDFLAGS := $(MODE_CFLAGS) \
           -s MODULARIZE=1 \
           -s EXPORT_NAME="createModule" \
           -s FORCE_FILESYSTEM \
           -s ASYNCIFY \
           -s EXPORTED_RUNTIME_METHODS=['FS','cwrap','ccall'] \
           -s EXPORTED_FUNCTIONS="['_main','_shell_init','_em_handle_command','_shell_interrupt','_checkpoint_sync_complete']" \
           -s ASYNCIFY_STACK_SIZE=10MB \
           -s STACK_SIZE=5MB \
           -s ALLOW_MEMORY_GROWTH=1 \
           -s USE_WEBGL2=1 \
           -lidbfs.js

# -- Static web assets --

STATIC_HTML   := $(WEB_DIR)/index.html
STATIC_JS     := $(wildcard $(WEB_DIR)/*.js)
STATIC_CSS    := $(wildcard $(WEB_DIR)/*.css)
STATIC_VENDOR := $(WEB_DIR)/vendor
STATIC_JS_DIR := $(WEB_DIR)/js

# -- Phony targets --

.PHONY: all release debug sanitize copy-static run \
        headless unit-test integration-test integration-test-valgrind \
        e2e-test test clean help FORCE

# -- WASM build --

all: $(OUTPUT) copy-static

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

# Link all objects into the final WASM module
$(OUTPUT): $(OBJ)
	@mkdir -p $(dir $@)
	@echo "Linking ($(MODE)) with $(CC)"
	$(CC) $(LDFLAGS) $(OBJ) -o $@

# Include auto-generated header dependency files
-include $(DEP)

# Copy web assets into the build directory
copy-static: $(STATIC_HTML) $(STATIC_JS) $(STATIC_CSS)
	@mkdir -p $(BUILD_DIR)
	@cp $(STATIC_HTML) $(BUILD_DIR)/
	@if [ -n "$(STATIC_JS)" ]; then cp $(STATIC_JS) $(BUILD_DIR)/; fi
	@if [ -n "$(STATIC_CSS)" ]; then cp $(STATIC_CSS) $(BUILD_DIR)/; fi
	@if [ -d "$(STATIC_VENDOR)" ]; then cp -R $(STATIC_VENDOR) $(BUILD_DIR)/; fi
	@if [ -d "$(STATIC_JS_DIR)" ]; then cp -R $(STATIC_JS_DIR) $(BUILD_DIR)/; fi
	@echo "Copied static assets."

# -- Run --
# Optional boot-media variables (paths relative to repo root):
#   ROM=path/to/rom.bin   FD0=path/to/floppy.img
#   HD0=path/to/hd.zip    HD1=...  (up to HD7)
#   SPEED=max|realtime|hardware

# Build the URL query string from media variables.
RUN_PARAMS :=
ifdef ROM
RUN_PARAMS += rom=/$(ROM)
endif
ifdef FD0
RUN_PARAMS += fd0=/$(FD0)
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

run: all
ifneq ($(strip $(RUN_PARAMS)),)
	@echo "Starting dev server on http://localhost:8080"
	python3 scripts/dev_server.py --root $(BUILD_DIR) --port 8080 $(RUN_SERVER_FLAGS) --default-params '$(RUN_QS)'
else
	@echo "Starting dev server on http://localhost:8080"
	python3 scripts/dev_server.py --root $(BUILD_DIR) --port 8080
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

# Run integration tests under Valgrind memcheck
integration-test-valgrind:
	$(MAKE) -C tests/integration test-valgrind

# Run Playwright end-to-end tests
e2e-test:
	cd tests/e2e && npx playwright test --config=playwright.config.ts

# Run unit + integration tests
test: unit-test integration-test

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
	@echo "  all (default)              Build WASM emulator (release)"
	@echo "  debug                      Build WASM emulator (debug)"
	@echo "  sanitize                   Build WASM emulator (sanitizers)"
	@echo "  headless                   Build native headless CLI"
	@echo "  run                        Build and start HTTP server on :8080"
	@echo ""
	@echo "Test targets:"
	@echo "  test                       Run unit + integration tests"
	@echo "  unit-test                  Build and run all unit tests"
	@echo "  integration-test           Build headless; run integration tests"
	@echo "  integration-test-valgrind  Integration tests under Valgrind"
	@echo "  e2e-test                   Run Playwright end-to-end tests"
	@echo ""
	@echo "Maintenance:"
	@echo "  clean                      Remove all build artifacts"
	@echo "  help                       Show this message"
	@echo ""
	@echo "Options:"
	@echo "  MODE=release|debug|sanitize  Build mode (default: release)"
	@echo "  EXTRA_CFLAGS=...             Additional compiler flags"
	@echo ""
	@echo "Boot media (for 'run' target):"
	@echo "  ROM=path/to/rom.bin          ROM image"
	@echo "  FD0=path/to/floppy.img       Floppy disk image"
	@echo "  HD0=path/to/hd.zip  ...HD7   Hard disk images (zip or raw)"
	@echo "  SPEED=max|realtime|hardware   Emulation speed"
	@echo ""
	@echo "Example:"
	@echo "  make run ROM=tests/data/roms/Plus_v3.rom HD0=tests/data/systems/hd.zip"
