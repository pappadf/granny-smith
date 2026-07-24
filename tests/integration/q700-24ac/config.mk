# Integration test: Quadra 700 + Display Card 24AC in NuBus slot $D —
# System 7.1 boots from SCSI HD to the Finder desktop (Phase F gate #1)
#
# First NuBus-card boot on the MCU family: the ROM's Slot Manager finds the
# card's declaration ROM at the top of slot-$D space (CRC pass needs the
# multi-entry host-region bus map), runs its declROM driver, and the System
# brings the card up as the SECOND display (640x480x8) next to the built-in
# DAFB.  ScrnBase stays on the DAFB — machine.screen shows the factory
# display, per the built-in-wins primary-display rule.

TEST_NAME := Quadra 700 NuBus 24AC second display
TEST_DESC := Q700 + 24AC in slot D boots System 7.1 to the desktop; card at 640x480x8

TEST_ROM := roms/q700-q900-420dbff3.rom

# Copy the HD image: the System writes to the volume during boot.
TEST_SETUP := cp "$(TEST_DATA)/systems/system_7_1_20mb_24ac_cd_32bit_gc.img" "$(WORK_DIR)/hd.img"

TEST_ARGS := model=q700 ram=8192
