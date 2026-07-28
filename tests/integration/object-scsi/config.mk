# Media: system_6_0_8_20mb_8_24gc.img, not hd1.zip. Verified 2026-07-27:
# hd1 is the same System 6.0.8 at the same 21,411,840-byte ST225N geometry,
# and the GC image's System Folder is a strict superset (AppleShare
# included), so every geometry/catalog assert holds unchanged — and this
# removes the suite's last TEST_SETUP unzip (§6.1).
# Integration test: scsi object class (M7d)
# Boots Plus with a SCSI HD attached so devices.N is non-empty.

TEST_NAME := Object-model SCSI class
TEST_DESC := scsi.bus.phase + scsi.devices indexed children + scsi.loopback

TEST_ROM := roms/plus-v3-4d1f8172.rom
TEST_SETUP := cp "$(TEST_DATA)/systems/system_6_0_8_20mb_8_24gc.img" "$(TEST_TMPDIR)/hd.img"
TEST_ARGS := hd=$(TEST_TMPDIR)/hd.img

# CI tier (proposal-integration-test-rework §5.4): unit | matrix | extended
TEST_TIER := unit
