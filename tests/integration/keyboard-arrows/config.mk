# Integration test: the arrow keys by name press the arrows (I1, N-34).
#
# Key identity across the model is the ADB raw keycode.  The name resolver
# (and the Lisa's keymap) keyed the arrows on the Mac OS virtual codes
# $7B-$7E, which on the ADB wire are the right-hand modifiers: on every ADB
# Mac, keyboard.press "left" pressed Right Shift.  This row reads the ROM's
# KeyMap (low memory $174-$183, one bit per virtual key code) at the ROM's
# boot-disk wait.

TEST_NAME := Keyboard arrows
TEST_DESC := keyboard.down "left"/"right"/"down"/"up" set the arrow bits of KeyMap, not a modifier's (iicx, lisa)

TEST_ROM := roms/iix-iicx-se30-97221136.rom
TEST_ARGS := model=iicx ram=8192

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
