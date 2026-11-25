# Integration suite: the V8 family (Macintosh LC II, Classic II, Color Classic)
#
# One run; rows re-instantiate via machine.boot.  Every row boots to the
# Finder, opens About This Macintosh from the Apple menu and asserts the
# machine id, the ROM id and the ROM's box flag ($0CB3).  Per model an older
# and a newer System, one from floppy and one from the SCSI disk:
#
#   lcii-701-fd     LC II, 7.0.1 floppy (7.0.x names it "Macintosh LC": the
#                   LC II returns the LC's machine type before 7.1)
#   lcii-71-hd      LC II, 7.1 HD ("Macintosh LC II")
#   lcii-753-hd     LC II, 7.5.3 HD
#   clii-701-fd     Classic II, 7.0.1 floppy
#   clii-71-hd      Classic II, 7.1 HD
#   ccl-701-fd      Color Classic, 7.0.1 floppy: refused at startup, as
#                   on the real machine (it needs 7.1 with its enabler)
#   ccl-753-hd      Color Classic, 7.5.3 HD, 512x384 colour
#   ccl-76-hd       Color Classic, 7.6 HD
#
#   make integration-test-suite-v8
#   make integration-test-suite-v8 TEST_VARS="ROW=lcii-71-hd"   one row only
#   make integration-test-suite-v8 TEST_VARS="REGEN=1"          recapture goldens

TEST_NAME := V8 family suite
TEST_DESC := LC II, Classic II and Color Classic: 7.0.1 floppy, 7.1 / 7.5.3 / 7.6 HD boots to the Finder and About This Macintosh

TEST_ROM := roms/lcii-35c28f5f.rom
TEST_ARGS := model=lcii ram=10240

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := matrix
