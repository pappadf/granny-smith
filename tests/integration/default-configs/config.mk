# Integration test: every model boots its default configuration.
#
# A machine.boot with nothing but the model and its ROM builds the model's
# default document (catalog.default_config): its storage devices, cards and
# connected display.  For every model this boots it, checks the machine has
# the default document's storage devices and a screen, and runs the ROM far
# enough that the screen shows something -- the guest reached its first
# display.

TEST_NAME := Default configurations boot
TEST_DESC := Every model boots catalog.default_config: its storage, a connected screen, and a ROM that draws on it

TEST_ROM := roms/plus-v3-4d1f8172.rom
TEST_ARGS := model=plus

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := matrix
