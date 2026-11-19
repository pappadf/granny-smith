# pdgen.mk — generates the predecoded cores' flat T1 id/case headers.
#
# Included by Makefile (wasm), Makefile.headless and tests/unit/common.mk:
# the headers are derived from the two decode trees by
# scripts/gen_pd_cases.py (docs/internals/core/cpu/predecode.md)
# into one shared output tree under build/gen/, and every core file that
# instantiates a predecoded executor includes them (-I$(PDGEN_OUT)).
#
# Paths are relative to the repository root; an includer elsewhere (the
# unit tier) sets GS_ROOT to that root, with a trailing slash.

PDGEN_SAVED_GOAL := $(.DEFAULT_GOAL)

GS_ROOT ?=
PDGEN_OUT := $(GS_ROOT)build/gen
PDGEN_SCRIPT := $(GS_ROOT)scripts/gen_pd_cases.py
PDGEN_CPU_TREE := $(GS_ROOT)src/core/cpu/cpu_decode.h
PDGEN_PPC_TREE := $(GS_ROOT)src/core/cpu/ppc/ppc_decode.h
PDGEN_CPU_HEADERS := $(PDGEN_OUT)/cpu_pd_t1_ids.h $(PDGEN_OUT)/cpu_pd_t1_classify.h $(PDGEN_OUT)/cpu_pd_t1_cases.h $(PDGEN_OUT)/cpu_pd_t1_names.h
PDGEN_PPC_HEADERS := $(PDGEN_OUT)/ppc_pd_t1_ids.h $(PDGEN_OUT)/ppc_pd_t1_classify.h $(PDGEN_OUT)/ppc_pd_t1_cases.h $(PDGEN_OUT)/ppc_pd_t1_names.h
PDGEN_HEADERS := $(PDGEN_CPU_HEADERS) $(PDGEN_PPC_HEADERS)

# The objects that instantiate a predecoded executor, under an includer's
# object root for src/core ($(1)); each depends on its architecture's set.
pdgen_cpu_objs = $(addprefix $(1)/cpu/,cpu.o cpu_68000.o cpu_68030.o cpu_68040.o)
pdgen_ppc_objs = $(addprefix $(1)/cpu/ppc/,ppc.o ppc_run.o)

# T1 ids start right after the shared control ids (PD_CONTROL_END = 16).
$(PDGEN_CPU_HEADERS) &: $(PDGEN_CPU_TREE) $(PDGEN_SCRIPT)
	@mkdir -p $(PDGEN_OUT)
	python3 $(PDGEN_SCRIPT) --tree $(PDGEN_CPU_TREE) --prefix cpu --base 16 --out $(PDGEN_OUT)

$(PDGEN_PPC_HEADERS) &: $(PDGEN_PPC_TREE) $(PDGEN_SCRIPT)
	@mkdir -p $(PDGEN_OUT)
	python3 $(PDGEN_SCRIPT) --tree $(PDGEN_PPC_TREE) --prefix ppc --base 16 --out $(PDGEN_OUT)

.DEFAULT_GOAL := $(PDGEN_SAVED_GOAL)
