# Integration test: the tnt-voodoo2-glide launch on the NORMATIVE
# synchronous walker, from a checkpoint of the launch (see test.script).
#
# The same media and the same flow as tnt-voodoo2-glide, from the same
# saved launch: the sibling runs the build's default backend, the worker
# thread, and between the two the claim is equivalence -- the walker's
# in-game frame, counters and LFB reads are byte-identical to the thread's.
# Queue order is submission order and every observation point fences, so
# this is the acceptance criterion, not a hope.
#
# MEDIA-GATED like its sibling; skips cleanly without the Quake image.

TEST_NAME := TNT Voodoo2 Glide (software walker)
TEST_DESC := The Quake launch, restored from a checkpoint, played on raster=sw, the normative walker, against the thread backend's frame from the same checkpoint

TEST_ROM := roms/pm7500-pm8500-pm9500-96cd923d.rom

TEST_SETUP := test ! -f "$(TEST_DATA)/apps/quake_8_1_voodoo2.img" || cp "$(TEST_DATA)/apps/quake_8_1_voodoo2.img" "$(WORK_DIR)/quake.img"

TEST_ARGS := model=pm7500 ram=65536

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
# Not a fixture consumer of the sibling's launch, deliberately: a consumer
# starts only when its producer has finished, and the two Quake runs
# back to back (~460 + ~260 s pooled) set the CI floor, where side by side
# they cost one boot more.

TEST_TIER := extended
