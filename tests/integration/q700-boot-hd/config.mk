# Integration test: Quadra 700 boots System 7.1 from SCSI HD to the desktop
# (Phase E gate)
#
# Full 53C96 + TurboSCSI pseudo-DMA boot path: driver descriptor / partition
# map / boot blocks via the ROM's Duff's-device drains, then the System's
# SCSI Manager READ(10) traffic (DMA select paused in COMMAND phase, FIFO
# flush, CDB through the pseudo-DMA aperture).  Regression pin for the
# flush-preserves-paused-select fix: with the CDB dropped, boot stalled in
# a 64M-instruction ioErr retry loop after "Welcome to Macintosh" and the
# CommToolbox 'cmtb' resources never loaded (null dispatch at PC=$2E).

TEST_NAME := Quadra 700 System 7.1 SCSI HD boot to desktop
TEST_DESC := 53C96 hard-disk boot: About box pixel-match (machine identity)

TEST_ROM := roms/q700-q900-420dbff3.rom

# Copy the HD image into TEST_TMPDIR: the System writes to the volume during
# boot (desktop DB etc.); the source image stays untouched between runs.
TEST_SETUP := cp "$(TEST_DATA)/systems/system_7_1_20mb_24ac_cd_32bit.img" "$(TEST_TMPDIR)/hd.img"

TEST_ARGS := model=q700 ram=8192 hd=$(TEST_TMPDIR)/hd.img

# CI tier (proposal-integration-test-rework §5.4): unit | matrix | extended
TEST_TIER := matrix
