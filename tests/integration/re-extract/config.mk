# Integration test: tools/dump end-to-end.  Targets the AUX 3.0.1 hd image's
# Finder — partition1 of the image is the MacOS Apple_HFS partition (the
# AUX UFS root lives on partition4 and is not touched by this test).
# The Finder's data fork + resource fork + Finder info sidecar are first
# extracted via the emulator's storage.cp, then fed to ./tools/dump/dump.

TEST_NAME := dump end-to-end (AUX 3.0.1 HFS Finder)
TEST_DESC := Extract the AUX hd image's MacOS-partition Finder; verify deterministic per-resource output
TEST_ROM := roms/plus-v3-4d1f8172.rom
TEST_RUNNER := run.sh
