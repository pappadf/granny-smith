# Integration test: TNT MESH disk boot on the pm9500, ON THE CARD
# (proposal-pci-mach64-gx-spinnaker, milestone 2d).
#
# The tnt-hd-boot chain — driver match, mount, System 7.6 to the Finder —
# on the machine that has no onboard video at all.  Every pixel here is
# produced by the ATI Mach64 GX in socket A1: its CRTC supplies the
# display descriptor, its RGB514 supplies the palette, and its draw engine
# paints the desktop.  With a display card staged the Control/Chaos
# fallback stands down, so this is also the row that proves a 9500 whose
# only display comes from an expansion slot boots and draws correctly.
#
# The golden was captured once and inspected by hand (TESTING.md: goldens
# are reviewed, never REGEN'd to green).  What that inspection established,
# recorded here so nobody has to derive it again:
#
# The desktop dither is structurally IDENTICAL to tnt-hd-boot-8500's — same
# checkerboard, same phase, same pixel positions — but the two goldens do
# not share a single desktop pixel VALUE: this row renders the pair as
# (135,135,135) / (193,193,193), the 8500 renders it as (96,96,96) /
# (160,160,160).  That is expected, and it is worth knowing why before
# anyone "fixes" it:
#
#   * The pixel INDICES are $81 and $F8.  In Apple's standard 8-bit CLUT
#     those are the neutral cube entry (102,102,102) and the fourth gray
#     ramp entry (170,170,170) — so both machines are drawing the same two
#     logical colours, and the difference is entirely in the CLUT.
#   * A Mac video driver writes its CLUT through a gamma table, and the
#     ndrv in this card's own expansion ROM carries a different one from
#     Control's.  135/193 is 102/170 under roughly a 1.4 gamma.
#   * The RGB514 is an 8-bit DAC and this model keeps all eight bits
#     verbatim (mach64_refresh_clut is a straight byte copy).  The 8500's
#     numbers are its linear values with the low nibble dropped — 102 ->
#     96, 170 -> 160 — so the Control path is the lossy one, not this.
#
# In other words the two rows disagreeing about desktop grey is a fact
# about two different DACs and two different gamma tables, not a defect in
# either.

TEST_NAME := TNT MESH disk boot (pm9500 + Mach64 GX)
TEST_DESC := pm9500 with no onboard video: 7.6 boots to the Finder on the PCI display card

TEST_ROM := roms/pm7500-pm8500-pm9500-96cd923d.rom

# Copied into WORK_DIR because the System writes to the volume during boot.
TEST_SETUP := cp "$(TEST_DATA)/systems/system_7_6_170mb_24ac.img" "$(WORK_DIR)/hd.img"

TEST_ARGS := model=pm9500 ram=32768

# CI tier (proposal-integration-test-rework §5.4): unit | matrix | extended
TEST_TIER := matrix
