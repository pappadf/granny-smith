# Integration test: the tnt-voodoo2-glide flow on the worker-thread raster
# backend (proposal-voodoo2-raster-thread §7).
#
# The SAME script, the SAME media, the SAME golden as tnt-voodoo2-glide
# (quake-ingame.png is a symlink into that directory): the only
# difference is pci_option="raster=thread", so the row's whole claim is
# equivalence — the threaded backend's in-game frame, counters and
# LFB reads are byte-identical to the normative walker's.  Queue order
# is submission order and every observation point fences, so this is
# the acceptance criterion the proposal states, not a hope.
#
# MEDIA-GATED like its sibling; skips cleanly without the Quake image.

TEST_NAME := TNT Voodoo2 Glide (thread backend)
TEST_DESC := The Quake flow on raster=thread, byte-identical to the walker's golden

TEST_ROM := roms/pm7500-pm8500-pm9500-96cd923d.rom

TEST_SETUP := test ! -f "$(TEST_DATA)/apps/quake_8_1_voodoo2.img" || cp "$(TEST_DATA)/apps/quake_8_1_voodoo2.img" "$(WORK_DIR)/quake.img"

TEST_ARGS := model=pm7500 ram=65536

# CI tier (proposal-integration-test-rework §5.4): unit | matrix | extended
TEST_TIER := extended
