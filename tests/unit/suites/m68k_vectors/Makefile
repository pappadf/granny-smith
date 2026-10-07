# 68K conformance-vector replay: third-party/m68k-test vectors through its
# reference runner, with the 68000/68030/68040 cores as the backend.  The 68K
# counterpart of suites/ppc_vectors/, and the sail-model sibling of
# suites/cpu/ (which replays the MAME-derived SingleStepTests for the 68000).
#
# The runner's sources are compiled straight out of the submodule — it is
# the executable definition of the replay contract (sparse inputs, the three
# output categories, the randomization envelope), so reimplementing it would
# be reimplementing the spec.  Two symbols are renamed below: its main(), so
# test.c can own main() and supply the tier paths the orchestrator cannot
# pass; and its default backend constructor, because the runner has no
# backend registry — the backend it builds by default IS the one under test.

TEST_NAME := m68k_vectors
TEST_SRCS := test.c
TEST_HARNESS := cpu

# The vector files are gzipped JSON; the runner reads them with zlib.
LDFLAGS += -lz

RUNNER := ../../../../third-party/m68k-test/runners/reference

EXTRA_SRCS := $(RUNNER)/src/main.c \
              $(RUNNER)/src/json.c \
              $(RUNNER)/src/state.c \
              $(RUNNER)/src/rng.c \
              $(RUNNER)/src/vector.c \
              $(RUNNER)/src/compare.c

EXTRA_CFLAGS := -I$(abspath $(RUNNER)/include) \
                -I$(abspath $(RUNNER)/src)

include ../../common.mk

# The runner ships a CLI; we link it as a library plus our own entry point,
# and its dry-run backend slot becomes the emulator backend (test.c).
$(OBJ_DIR)/third-party/m68k-test/runners/reference/src/main.o: EXTRA_CFLAGS += -Dmain=m68k_runner_main -Dbackend_dryrun=m68k_backend_custom

# The tier the binary replays: the submodule's smoke tier minus the known
# core-vs-model disagreements in exclusions.txt (filter.py says what was
# dropped and why).  `run` rebuilds it when the tier or the list changes;
# the binary finds it through M68K_VECTORS_DIR (exported to the recipe).
TIER     := $(abspath $(RUNNER)/../../vectors/smoke)
FILTERED := $(BUILD_DIR)/m68k_vectors.tier
export M68K_VECTORS_DIR := $(FILTERED)

$(FILTERED)/.stamp: filter.py exclusions.txt $(wildcard $(TIER)/*/*.json.gz)
	python3 filter.py --tier $(TIER) --exclusions exclusions.txt --out $(FILTERED)
	@touch $@

run: $(FILTERED)/.stamp
