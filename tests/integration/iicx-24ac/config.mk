# Integration test configuration: IIcx + Display Card 24AC, 32-bit SCSI cdev boot
#
# Boots the IIcx with the Display Card 24AC and 8 MB RAM from the pre-built
# 20 MB System 7.1 SCSI HD image that has the 24AC cdev installed and 32-bit
# addressing enabled (systems/system_7_1_20mb_24ac_cd_32bit.img).  Reaches an
# 8-bpp COLOUR Finder desktop (640x480) and matches it pixel-exact.  This is the
# proposal's Step B (the cdev binds at boot and the accelerator engages) as an
# end-to-end boot, complementing iicx-display-card-24ac's floppy boot +
# engine-decode/oracle coverage.
#
# The image is stored in gs-test-data as systems/system_7_1_20mb_24ac_cd_32bit.img.7z
# and auto-extracted to systems/system_7_1_20mb_24ac_cd_32bit.img by
# scripts/fetch-test-data.sh (no TEST_SETUP needed — the .7z extraction is generic).

TEST_NAME := IIcx Display Card 24AC — 32-bit SCSI cdev boot
TEST_DESC := Boot IIcx + 24AC from the 32-bit System 7.1 SCSI cdev image to an 8-bpp colour Finder desktop

TEST_ROM := roms/IIcx.rom

# The harness creates the IIcx with 8 MB; the script re-boots with the 24AC
# card selected (video_card can't be passed as an arg) and attaches the SCSI HD.
TEST_ARGS := model=iicx ram=8192
