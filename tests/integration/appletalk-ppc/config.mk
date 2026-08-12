# Integration test configuration: PPC Toolbox program linking against a real
# System 7 guest (proposal-appletalk-ppc-appleevents.md WP-8).
#
# The guest is the 20 MB System 7.1 SCSI image, booted on the Plus.  It is the
# only machine family with the AppleTalk stack wired in (appletalk_init is
# called from plus.c alone), and this image boots there and brings its
# AppleTalk driver up, which is what the test needs.
#
# Program linking is not enabled in the shipped image, so the script turns it
# on the way a user would: Sharing Setup for the identity and the Program
# Linking switch, Users & Groups for guest access.  That is a few hundred
# million instructions of choreography, but it keeps the suite self-contained
# — no separately prepared media, and the setup itself is a regression test
# for the control panels.
#
# Assertions are protocol state, not pixels: the point of this feature is the
# semantic channel, and a choreography that drifts fails loudly anyway because
# nothing gets discovered.

TEST_NAME := AppleTalk PPC program linking
TEST_DESC := Boot System 7.1, enable program linking, discover the guest's PPC ports over NBP + list-ports, and open an Apple event session to the Finder.

TEST_ROM := roms/plus-v3-4d1f8172.rom

# A private copy: the guest writes its Sharing Setup and Users & Groups
# changes, and a shared image would carry them between runs.
TEST_SETUP := cp "$(TEST_DATA)/systems/system_7_1_20mb_24ac_cd_32bit_gc.img" "$(TEST_TMPDIR)/hd.img"

TEST_ARGS := ram=4096 hd=$(TEST_TMPDIR)/hd.img

# CI tier (proposal-integration-test-rework §5.4): unit | matrix | extended
TEST_TIER := extended
