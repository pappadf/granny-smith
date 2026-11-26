# Integration suite: the Workgroup Server 9150 (PDM family: the 8100's
# board in a server tower, with its own CPU identification and a fourth
# NuBus slot)
#
# One daemon run; rows re-instantiate via machine.boot (the suite-pdm
# shape).  The 9150/80 and 9150/120 each have their own ROM; rows name the
# 120's explicitly.
#
#   make integration-test-suite-wgs9150
#   make integration-test-suite-wgs9150 TEST_VARS="ROW=wgs9150-753-hd"   one row only
#   make integration-test-suite-wgs9150 TEST_VARS="KEEP_GOING=1"         nightly mode
#   make integration-test-suite-wgs9150 TEST_VARS="REGEN=1"              recapture goldens

TEST_NAME := Workgroup Server 9150 suite (9150/80, 9150/120)
TEST_DESC := System 7.5.3 and 7.6 SCSI boots to the Finder + About box on both 9150 models, a 7.6 floppy boot, and a display card in the fourth NuBus slot ($A)

# 4 MB Workgroup Server 9150/80 ROM (stored checksum 0x9C7C98F7).
TEST_ROM := roms/wgs9150-80mhz-9c7c98f7-e9220fe5992ffdf2.rom

TEST_ARGS := model=wgs9150 ram=32768

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := matrix
