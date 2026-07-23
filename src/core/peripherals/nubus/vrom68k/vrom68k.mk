# vrom68k.mk — assembles the GS declaration-ROM 68K fragments.
#
# Included by BOTH Makefile (wasm) and Makefile.headless: the fragments
# are target-independent data (raw 68K blocks embedded as C arrays), so
# one shared output tree under build/vrom68k/ serves every build.
#
# Requires GNU binutils targeting m68k (binutils-m68k-linux-gnu on
# Ubuntu/Debian, 2.42+ verified; shipped in the devcontainer image).
# Any m68k-targeted as/objcopy works — override M68K_AS/M68K_OBJCOPY.
# There is deliberately NO fallback when the assembler is missing:
# builds whose content depends on the environment are worse than a loud
# failure (runtime-vrom proposal §3.2).

# This file is included before the including Makefile's first target;
# save and restore the default goal so our header rule doesn't hijack it.
VROM68K_SAVED_GOAL := $(.DEFAULT_GOAL)

VROM68K_DIR := src/core/peripherals/nubus/vrom68k
VROM68K_OUT := build/vrom68k
VROM68K_HEADER := $(VROM68K_OUT)/gsvrom_fragments.h

M68K_AS      ?= m68k-linux-gnu-as
M68K_OBJCOPY ?= m68k-linux-gnu-objcopy
# --register-prefix-optional keeps the source in plain Motorola register
# spelling (a4, d0, sp) rather than gas's %-prefixed form.
M68K_ASFLAGS := -m68030 --register-prefix-optional -I $(VROM68K_DIR)

# Per-personality assembler symbol.
VROM68K_SYM_jmfb   := PERSONALITY_JMFB
VROM68K_SYM_boogie := PERSONALITY_BOOGIE
VROM68K_SYM_mdcgc  := PERSONALITY_MDCGC
VROM68K_SYM_se30   := PERSONALITY_SE30

# The fragment matrix: PrimaryInit + DRVR for all four personalities,
# SecondaryInit for the GC only.
VROM68K_FRAGS := jmfb_init jmfb_drvr boogie_init boogie_drvr \
                 mdcgc_init mdcgc_drvr mdcgc_sinit se30_init se30_drvr
VROM68K_BINS  := $(foreach f,$(VROM68K_FRAGS),$(VROM68K_OUT)/frag_$(f).bin)

VROM68K_SRC_DEPS := $(wildcard $(VROM68K_DIR)/*.s) $(wildcard $(VROM68K_DIR)/*.i)

# frag_<pers>_<block>.bin: assemble frag_<block>.s with the personality
# symbol, then flatten (objcopy -O binary is safe: every reference is a
# same-section difference, so no relocations survive to be dropped).
$(VROM68K_OUT)/frag_%.bin: $(VROM68K_SRC_DEPS)
	@command -v $(M68K_AS) >/dev/null 2>&1 || { \
	  echo "error: $(M68K_AS) not found — install binutils-m68k-linux-gnu" >&2; \
	  echo "       (or point M68K_AS/M68K_OBJCOPY at another m68k binutils)" >&2; \
	  exit 1; }
	@mkdir -p $(VROM68K_OUT)
	$(M68K_AS) $(M68K_ASFLAGS) --defsym $(VROM68K_SYM_$(word 1,$(subst _, ,$*)))=1 \
	  -o $(VROM68K_OUT)/frag_$*.o $(VROM68K_DIR)/frag_$(word 2,$(subst _, ,$*)).s
	$(M68K_OBJCOPY) -O binary $(VROM68K_OUT)/frag_$*.o $@

# The generated header gsvrom_data.c includes (-I$(VROM68K_OUT)).
$(VROM68K_HEADER): $(VROM68K_BINS) scripts/bin2c.py
	python3 scripts/bin2c.py --out $@ --guard GSVROM_FRAGMENTS_H \
	  $(foreach f,$(VROM68K_FRAGS),gsvrom_frag_$(f)=$(VROM68K_OUT)/frag_$(f).bin)

# Restore the including Makefile's default goal (empty re-arms "first
# target defined next becomes the goal").
.DEFAULT_GOAL := $(VROM68K_SAVED_GOAL)
