# Integration test: skeleton re.dump (PR 2 of the resource-fork-as-VFS-tree
# proposal).  Targets the AUX 3.0.1 hd image's Finder — this is the
# canonical RE subject for the proposal.  partition1 of the image is the
# MacOS Apple_HFS partition (the AUX UFS root lives on partition4 and is
# not touched by this test).

TEST_NAME := re.dump skeleton (AUX 3.0.1 HFS Finder)
TEST_DESC := Extract the AUX hd image's MacOS-partition Finder; verify deterministic per-resource output
TEST_ROM := roms/Plus_v3.rom
TEST_RUNNER := run.sh
