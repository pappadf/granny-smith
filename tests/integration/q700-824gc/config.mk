# Integration test: Quadra 700 + Display Card 8•24 GC in NuBus slot $D —
# System 7.1 boots from SCSI HD to the Finder desktop (Phase F gate #2)
#
# The GC card in a Quadra: the declROM driver runs (mode set, CLUT writes,
# slot VBL interrupts through the VIA2 PA4 + /SLOTIRQ aggregate) and the
# card works as an UNACCELERATED second display.  The GC acceleration
# software checks the CPU and declines to install on a 68040 — matching
# Apple's shipped behavior (the GC INIT never touches the coprocessor on
# Quadras), so gc.on stays false by design; the same image on a IIcx turns
# it on.  Boot to the desktop with the card's slot VBL live is the gate.

TEST_NAME := Quadra 700 NuBus 8•24 GC second display
TEST_DESC := Q700 + 8•24 GC in slot D boots System 7.1 to the desktop; GC INIT declines on 040

TEST_ROM := roms/q700-q900-420dbff3.rom

# Copy the HD image: the System writes to the volume during boot.
TEST_SETUP := cp "$(TEST_DATA)/systems/system_7_1_20mb_24ac_cd_32bit_gc.img" "$(WORK_DIR)/hd.img"

TEST_ARGS := model=q700 ram=8192

# CI tier (proposal-integration-test-rework §5.4): unit | matrix | extended
TEST_TIER := matrix
