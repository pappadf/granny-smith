# Makefile — builds the GS generic NuBus declaration ROM blobs.
#
# Products (under build/vrom/):
#   gsvrom-jmfb.bin / -boogie.bin / -mdcgc.bin / -se30.bin   — 32 KB images
#   gsvrom_blobs.h                                           — embedded C arrays
#
# One assembly source tree, four personalities selected by -DPERSONALITY_*
# (proposal-generic-nubus-vrom.md sec. 3.2).  The assembler (vasm) is fetched
# and built on demand by fetch-vasm.sh, cached under build/vasm/.

ROOT      := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
OUT       := $(ROOT)/build/vrom
VASM      := $(ROOT)/build/vasm/vasmm68k_mot
PYTHON    ?= python3

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
# neither vasm, python, nor network); this Makefile regenerates it in place
# and the diff is reviewable (proposal sec. 8).
BLOBS_H   := $(ROOT)/src/core/peripherals/nubus/gsvrom_blobs.h

BLOBS     := $(foreach p,$(PERSONALITIES),$(OUT)/gsvrom-$(p).bin)

.PHONY: all clean
all: $(BLOBS_H)

# Assembler: fetched + built once, cached in build/vasm/.
$(VASM):
	$(ROOT)/tools/vrom/fetch-vasm.sh

# Assemble one personality to a raw binary, then stamp Length/CRC/ByteLanes
# and pad to the chip size.
$(OUT)/gsvrom-%.bin: $(SRC) $(INCLUDES) crc.py | $(VASM)
	mkdir -p $(OUT)
	$(VASM) -Fbin -m68030 -nosym -D$(SYM_$*)=1 -o $(OUT)/gsvrom-$*.raw $(SRC)
	$(PYTHON) crc.py $(OUT)/gsvrom-$*.raw $@ --byte-lanes $(BYTELANES) --max-size $(MAX_SIZE)

# Embed the four blobs as C arrays in the committed generated header.
$(BLOBS_H): $(BLOBS) embed.py
	$(PYTHON) embed.py --out $@ $(foreach p,$(PERSONALITIES),$(p)=$(OUT)/gsvrom-$(p).bin)

clean:
	rm -rf $(OUT)
