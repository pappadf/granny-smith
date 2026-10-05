# Integration test: memory logpoints survive a machine re-mapping their page
# (#234).
#
# A memory logpoint keeps its page's SoA fast-path entries at 0, so every
# access takes the slow path and reaches the hook.  The machines' own direct
# page writers (here the Plus's plus_map_read_page, through the ROM overlay)
# re-installed a direct host mapping without checking, so a logpoint armed
# before the overlay dropped -- or before a machine.reset, which brings it
# back -- silently stopped firing.

TEST_NAME := Memory logpoints survive page re-maps
TEST_DESC := a Ticks read logpoint armed before boot, and kept across machine.reset, keeps firing on a Plus

TEST_ROM := roms/plus-v3-4d1f8172.rom

TEST_ARGS := model=plus

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
