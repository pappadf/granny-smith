# Integration test: one monitor sense per display device.
#
# A IIci with a Display Card 8•24 in NuBus slot 4 has two display devices:
# the built-in video (the RBV) and the card.  Each gets its own monitor from
# the boot document's displays, and a device with none plugged in reads
# "nothing connected" on its sense lines, so the ROM and the System turn it
# off.  The guest's GDevice list under System 7.0.1 is the proof: one screen
# when one monitor is plugged in, whichever device has it, against two for
# the legacy arguments, which leave the card's default monitor plugged in
# beside the built-in one.

TEST_NAME := Per-device monitor sense (IIci + 8•24)
TEST_DESC := Built-in video off with the card connected, the card off with the built-in video connected, both on with the legacy arguments

TEST_ROM := roms/iici-368cadfe.rom
TEST_ARGS := model=iici ram=8192

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := matrix
