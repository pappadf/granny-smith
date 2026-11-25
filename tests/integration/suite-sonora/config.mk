# Integration suite: the Sonora boards of the V8 family (Macintosh LC III,
# LC III+, LC 520, LC 550, Color Classic II)
#
# One run; rows re-instantiate via machine.boot.  Every row boots to the
# Finder, opens About This Macintosh from the Apple menu and asserts the
# machine id, the ROM id and the ROM's box flag ($0CB3).  These boards need
# System 7.1 with their System Enablers (not in the test data) or 7.5 and
# later, which name every 68K model "Macintosh" in About.
#
#   lciii-753-hd      LC III, 7.5.3 HD (box 21)
#   lciii-76-fd       LC III, 7.6 Disk Tools floppy
#   lciii-early-76-hd LC III on the early ROM ($EC904829), 7.6 HD
#   lciiip-753-hd     LC III+, 7.5.3 HD (box 56)
#   lc520-753-hd      LC 520, 7.5.3 HD (box 50)
#   lc550-76-hd       LC 550, 7.6 HD (box 74)
#   cc2-753-hd        Color Classic II, 7.5.3 HD (box 77)
#   cc2-76-fd         Color Classic II, 7.6 Disk Tools floppy
#   lciii-71-refused  LC III, 7.1 HD: refused at startup without its enabler
#   lciii-12in-checkpoint  LC III on the 12" RGB at 20 MB (a two-bank SIMM
#                     stitched above the soldered RAM), then a checkpoint
#                     round-trip through an LC 550 boot
#
#   make integration-test-suite-sonora
#   make integration-test-suite-sonora TEST_VARS="ROW=lc520-753-hd"   one row only
#   make integration-test-suite-sonora TEST_VARS="REGEN=1"            recapture goldens

TEST_NAME := Sonora family suite
TEST_DESC := LC III, LC III+, LC 520, LC 550 and Color Classic II: 7.5.3 / 7.6 HD and 7.6 floppy boots to the Finder and About This Macintosh

TEST_ROM := roms/lciii-ecbbc41c.rom
TEST_ARGS := model=lciii ram=8192

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := matrix
