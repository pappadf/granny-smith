# Makefile — builds the GS generic NuBus declaration ROM blobs.
#
# Products (under build/vrom/):
#   gsvrom-jmfb.bin / -boogie.bin / -mdcgc.bin / -se30.bin   — 32 KB images
#   gsvrom_blobs.h                                           — embedded C arrays
#
# One assembly source tree, four personalities selected by
# --defsym PERSONALITY_*=1 (proposal-generic-nubus-vrom.md sec. 3.2).
# The assembler is GNU binutils targeting m68k (runtime-vrom proposal
# stage A): any m68k-targeted as/objcopy pair works because the output
# is a raw binary — Debian/Ubuntu's binutils-m68k-linux-gnu package, an
# m68k-elf cross build on macOS, etc.  Override the tool names with
# M68K_AS= / M68K_OBJCOPY= if yours differ.

ROOT      := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
OUT       := $(ROOT)/build/vrom
PYTHON    ?= python3

M68K_AS      ?= m68k-linux-gnu-as
M68K_OBJCOPY ?= m68k-linux-gnu-objcopy
# --register-prefix-optional keeps the source in plain Motorola register
# spelling (a4, d0, sp) rather than gas's %-prefixed form.
M68K_ASFLAGS := -m68030 --register-prefix-optional

# Chip geometry: dense 4-lane images for every personality (sec. 5.3).  The
# image is NOT padded to a power-of-two chip size: like the real 8•24 ROM
# (Length $4000 inside a 32 KB chip), Length/CRC cover exactly the assembled
# content, and the loader tail-places the image at the top of the slot
# window, so only the meaningful bytes need to exist (or be embedded).
BYTELANES := 0x0F
MAX_SIZE  := 32768

PERSONALITIES := jmfb boogie mdcgc se30

# Per-personality assembler symbol.
SYM_jmfb   := PERSONALITY_JMFB
SYM_boogie := PERSONALITY_BOOGIE
SYM_mdcgc  := PERSONALITY_MDCGC
SYM_se30   := PERSONALITY_SE30

SRC       := gsvrom.s
INCLUDES  := $(wildcard *.i) $(wildcard gsvrom_*.s) $(wildcard ops_*.s)

# The generated blobs header is COMMITTED (so ordinary emulator builds need
# neither the m68k toolchain nor python); this Makefile regenerates it in
# place and the diff is reviewable (proposal sec. 8).
BLOBS_H   := $(ROOT)/src/core/peripherals/nubus/gsvrom_blobs.h

BLOBS     := $(foreach p,$(PERSONALITIES),$(OUT)/gsvrom-$(p).bin)

.PHONY: all clean check-as
all: $(BLOBS_H)

# Fail early with an actionable message when the cross assembler is absent.
check-as:
	@command -v $(M68K_AS) >/dev/null 2>&1 || { \
	  echo "error: $(M68K_AS) not found — install binutils-m68k-linux-gnu" >&2; \
	  echo "       (or point M68K_AS/M68K_OBJCOPY at another m68k binutils)" >&2; \
	  exit 1; }

# Assemble one personality to a raw binary (as + objcopy -O binary; every
# reference is a same-section difference so no relocations survive to be
# dropped — verified by the empty `objdump -r` in the port probes), then
# stamp Length/CRC/ByteLanes.
$(OUT)/gsvrom-%.bin: $(SRC) $(INCLUDES) crc.py | check-as
	mkdir -p $(OUT)
	$(M68K_AS) $(M68K_ASFLAGS) --defsym $(SYM_$*)=1 -o $(OUT)/gsvrom-$*.o $(SRC)
	$(M68K_OBJCOPY) -O binary $(OUT)/gsvrom-$*.o $(OUT)/gsvrom-$*.raw
	$(PYTHON) crc.py $(OUT)/gsvrom-$*.raw $@ --byte-lanes $(BYTELANES) --max-size $(MAX_SIZE)

# Embed the four blobs as C arrays in the committed generated header.
$(BLOBS_H): $(BLOBS) embed.py
	$(PYTHON) embed.py --out $@ $(foreach p,$(PERSONALITIES),$(p)=$(OUT)/gsvrom-$(p).bin)

clean:
	rm -rf $(OUT)
