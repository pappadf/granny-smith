# Integration test configuration: Power Macintosh 7100 — boot Copland D11E4.
#
# The last phase of the Copland bring-up (bring-up plan §6.3).  It starts from
# the volume `pm7100-copland-install-os8` publishes: Drive-Setup-2.0d5c2
# formatted, System 7.5.0 in `System Folder`, Mac OS 8 D11E4 in `Mac OS Folder`.
#
# Row-per-milestone rather than one linear script, so partial progress is
# visible instead of one red light.  `row_control_75` is not optional: a boot
# WITHOUT the Caps Lock latch must still reach the 7.5 Finder, because without
# that control "Copland did not boot" and "the image is broken" look identical.

TEST_NAME := Power Macintosh 7100 — boot Copland D11E4 (Caps Lock)
TEST_DESC := Boot the installed volume with and without the Caps Lock latch, and track how far Copland's loader gets

TEST_ROM := roms/pm6100-pm7100-pm8100-9feb69b3.rom
TEST_ARGS := model=pm7100 ram=24576
TEST_TIER := extended
