# Integration test: the keyboard-and-mouse node says what the machine has.
#
# `machine.adb` is the one machine-independent path to the keyboard and the
# mouse, on every model.  What the node SHOWS -- its label in the SYSTEM tab
# and in `meta.members`, and its doc -- follows the machine: "ADB" only on a
# machine with an Apple Desktop Bus, the Lisa's COPS on a Lisa, and a neutral
# "Keyboard & Mouse" on a Plus (M0110 keyboard on the VIA, quadrature mouse)
# and with no machine at all.  The path keeps working everywhere.

TEST_NAME := Input node label per machine
TEST_DESC := machine.adb is labelled ADB only where there is an ADB bus (COPS on a Lisa, Keyboard & Mouse on a Plus); the path works on all

TEST_ROM := roms/plus-v3-4d1f8172.rom
TEST_ARGS := model=plus

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
