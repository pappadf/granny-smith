# Integration test: the drive-activity counters behind the status-bar lights.
# Every disk read and write the machine's drives make counts on the image
# (storage.images[i].reads / .writes); the web host samples the per-kind
# sums once per tick to light HD / FD / CD.

TEST_NAME := Drive-activity counters
TEST_DESC := storage.images[i].reads/writes count the drives' I/O

TEST_ROM := roms/plus-v3-4d1f8172.rom
TEST_ARGS := fd0=$(TEST_DATA)/systems/System_6_0_8.dsk

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
