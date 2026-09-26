# Integration test: a bare UFS volume -- an A/UX partition with no partition
# map around it -- mounts through the image VFS (09-storage F-45).
#
# The fixture is cut at setup time from partition 6, "UNIX Root&Usr slice
# 0", of the A/UX 3.0.1 CD (Apple_UNIX_SVR2, blocks 361632 + 274566) --
# the same filesystem image-ufs-traverse reads through the partition map --
# so no binary is committed.  Sparse, so the cut costs little disk.

TEST_NAME := storage.ufs-bare
TEST_DESC := Mount a bare A/UX UFS volume and read it like the same partition inside its disk

TEST_ROM := roms/iix-iicx-se30-97221136.rom

TEST_SETUP := dd if="$(TEST_DATA)/aux/aux_3.0.1/APPLE_AUX_3-0-1_RETAIL.iso" of="$(WORK_DIR)/aux-root.ufs" bs=512 skip=361632 count=274566 conv=sparse status=none

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
