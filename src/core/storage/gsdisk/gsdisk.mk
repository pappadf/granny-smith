# gsdisk.mk — assembles the GSDisk SCSI disk driver (the 68k boot driver
# the bare-volume wrapper puts in front of a naked HFS volume).
#
# Included by BOTH Makefile (wasm) and Makefile.headless, like vrom68k.mk:
# the driver is target-independent data, embedded as a C array, so one
# shared output tree under build/gsdisk/ serves every build.  Needs the
# same m68k binutils as vrom68k.mk (M68K_AS / M68K_OBJCOPY), and has no
# fallback when they are missing.

# Included before the including Makefile's first target; keep its goal.
GSDISK_SAVED_GOAL := $(.DEFAULT_GOAL)

GSDISK_DIR    := src/core/storage/gsdisk
GSDISK_OUT    := build/gsdisk
GSDISK_HEADER := $(GSDISK_OUT)/gsdisk_driver.h

M68K_AS      ?= m68k-linux-gnu-as
M68K_OBJCOPY ?= m68k-linux-gnu-objcopy
# 68000 only: one binary boots every 68k ROM, the Plus included.
GSDISK_ASFLAGS := -m68000 --register-prefix-optional

# Assemble, then flatten (no relocations: every reference is PC-relative
# or a same-section difference).  Temporary names + rename, so an
# interrupted or racing build never leaves a torn binary behind.
$(GSDISK_OUT)/gsdisk_drvr.bin: $(GSDISK_DIR)/gsdisk_drvr.s $(GSDISK_DIR)/gsdisk.mk
	@command -v $(M68K_AS) >/dev/null 2>&1 || { \
	  echo "error: $(M68K_AS) not found — install binutils-m68k-linux-gnu" >&2; \
	  echo "       (or point M68K_AS/M68K_OBJCOPY at another m68k binutils)" >&2; \
	  exit 1; }
	@mkdir -p $(GSDISK_OUT)
	$(M68K_AS) $(GSDISK_ASFLAGS) -o $(GSDISK_OUT)/gsdisk_drvr.o.$$$$ $< && \
	$(M68K_OBJCOPY) -O binary $(GSDISK_OUT)/gsdisk_drvr.o.$$$$ $@.$$$$ && \
	mv -f $(GSDISK_OUT)/gsdisk_drvr.o.$$$$ $(GSDISK_OUT)/gsdisk_drvr.o && mv -f $@.$$$$ $@

# The generated header image_wrap.c includes (-I$(GSDISK_OUT)).
$(GSDISK_HEADER): $(GSDISK_OUT)/gsdisk_drvr.bin scripts/bin2c.py
	python3 scripts/bin2c.py --out $@ --guard GSDISK_DRIVER_H gsdisk_drvr=$<

.DEFAULT_GOAL := $(GSDISK_SAVED_GOAL)
