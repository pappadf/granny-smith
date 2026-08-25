# Integration test configuration: Power Macintosh 7100 — boot MkLinux DR3.
#
# The keeper test, and the last phase of the MkLinux bring-up (bring-up plan
# §5.3).  It starts from the pair `pm7100-mklinux-install` publishes: a 7.5
# host volume whose lilo.conf names the installed root and whose MkLinux
# control panel defaults to MkLinux, and the target carrying the installed
# system.
#
# Row-per-milestone rather than one linear script, so partial progress is
# visible instead of one red light.  `row_control_macos` is not optional: a
# boot that clicks "Boot MacOS" must still reach the 7.5 Finder, because
# without that control "MkLinux broke" and "the volume broke" look identical.

TEST_NAME := Power Macintosh 7100 — boot MkLinux DR3 to a login prompt
TEST_DESC := Boot the installed volume through the MkLinux Booter, ride Mach_Kernel and the Linux server to the console login, and log in as root

TEST_ROM := roms/pm6100-pm7100-pm8100-9feb69b3.rom
TEST_ARGS := model=pm7100 ram=40960
TEST_TIER := extended
