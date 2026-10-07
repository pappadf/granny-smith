# Integration test: the machine configuration's ImageWriter option
#
# The configuration document's "imagewriter" option (none, imagewriter,
# imagewriter2; the dialog's "ImageWriter" menu) plugs the printer in at boot,
# on the serial port AppleTalk does not use: the modem port (SCC channel A)
# while AppleTalk is active, the printer port (channel B) when it is
# inactive, and Serial A on a Lisa.  Without the option the printer stays
# unplugged.  No guest code runs.

TEST_NAME := ImageWriter configuration option
TEST_DESC := config "imagewriter" plugs the chosen printer into the serial port AppleTalk leaves free, on a Plus and a Lisa.

TEST_ROM := roms/plus-v3-4d1f8172.rom

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
