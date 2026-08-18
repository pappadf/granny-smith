# PPC (MPC601) conformance-vector replay: third-party/powerpc-test vectors
# through the reference runner, with our core as its `custom` backend.
# The PowerPC counterpart of suites/cpu/ (SingleStepTests for the 68000).
#
# The runner's own sources are compiled straight out of the submodule — it
# is the executable definition of the replay contract (sparse inputs, the
# three output categories, the randomization envelope), so reimplementing it
# would be reimplementing the spec.  Its main() is renamed below so test.c
# can own main() and supply the tier path the orchestrator cannot pass.

TEST_NAME := ppc_vectors
TEST_SRCS := test.c
TEST_HARNESS := cpu

# The vector files are gzipped JSON; the runner reads them with zlib.
LDFLAGS += -lz

RUNNER := ../../../../third-party/powerpc-test/runners/reference

EXTRA_SRCS := ../../../../src/core/cpu/ppc/ppc.c \
              ../../../../src/core/cpu/ppc/ppc_run.c \
              ../../../../src/core/cpu/ppc/ppc_mmu.c \
              ../../../../src/core/cpu/ppc/ppc_fpu.c \
              ../../../../src/core/cpu/ppc/ppc_softfp.c \
              ../../../../src/core/cpu/ppc/ppc_disasm.c \
              $(RUNNER)/src/util.c \
              $(RUNNER)/src/json.c \
              $(RUNNER)/src/mem.c \
              $(RUNNER)/src/state.c \
              $(RUNNER)/src/rng.c \
              $(RUNNER)/src/vector.c \
              $(RUNNER)/src/envelope.c \
              $(RUNNER)/src/compare.c \
              $(RUNNER)/src/step_dryrun.c \
              $(RUNNER)/src/step_sail.c \
              $(RUNNER)/src/main.c

EXTRA_CFLAGS := -I$(abspath ../../../../src/core/cpu/ppc) \
                -I$(abspath $(RUNNER)/include)

include ../../common.mk

# The runner ships a CLI; we link it as a library plus our own entry point.
$(OBJ_DIR)/third-party/powerpc-test/runners/reference/src/main.o: EXTRA_CFLAGS += -Dmain=ppc_runner_main
