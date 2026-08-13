# Integration test configuration: Apple events against the Scriptable Finder
# (proposal-appletalk-ppc-appleevents.md §7.2, the flagship script).
#
# System 7.5 on a IIci — the combination this feature exists for.  7.5 brings
# the Scriptable Finder, and the IIci has the AppleTalk stack now that it is
# wired into every 68030 family rather than the Plus alone.
#
# The whole point is that the assertions are values, not pixels: the guest is
# asked for its Finder's version and its startup disk's name, and the replies
# are read out of the object model.  There is not a single screenshot here.

TEST_NAME := Apple events — Scriptable Finder
TEST_DESC := Boot System 7.5 on a IIci, enable program linking, and query the Scriptable Finder over Apple events for its version and its startup disk name.

TEST_ROM := roms/iici-368cadfe.rom

# A private copy: the guest writes its Sharing Setup and Users & Groups
# changes, and a shared image would carry them between runs.
TEST_SETUP := cp "$(TEST_DATA)/systems/system_7_5_0_77mb_mode32_24ac.img" "$(TEST_TMPDIR)/hd.img"

TEST_ARGS := model=iici ram=8192 hd=$(TEST_TMPDIR)/hd.img

# CI tier (proposal-integration-test-rework §5.4): unit | matrix | extended
TEST_TIER := extended
