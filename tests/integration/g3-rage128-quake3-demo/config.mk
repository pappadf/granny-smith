# Integration test: Quake III Arena's demo four on a Rage 128, timed
#
# The beige G3 with the Rage 128 GL as its only display and 256 MB,
# gs-test-data's stripped Mac OS 9.2.1 disk
# (systems/mac_os_9_2_1_516mb_g3_rage128_demos.dmg) and the Quake III 1.30
# CD cut down to the demo (apps/quake3_1_30_demo_four_cd.iso).  Quake runs
# from the CD and plays demos/four.dm_66 (q3dm6, eight players, 63 s); the
# row times it from the console command to its end, in host and guest
# time, with the frames drawn.

TEST_NAME := G3 Quake III demo on a Rage 128
TEST_DESC := Quake III Arena 1.30 plays its demo four through the Rage 128 GL's 3D engine to the end, timed in host and guest time with the frame rate

TEST_ROM := roms/pmg3dt-pmg3mt-78f57389.rom

TEST_ARGS := model=pmg3dt ram=262144

# The boot, Quake's start and the demo: ~75 G instructions.
TEST_TIMEOUT := 3600

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := extended
