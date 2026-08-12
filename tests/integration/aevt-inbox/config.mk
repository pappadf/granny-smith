# Integration test configuration: inbound Apple events (WP-7).
#
# The other direction from aevt-finder and aevt-stress: instead of driving the
# guest, this test has a guest application link to the port we advertise and
# send *us* an event, which we decode, record in `appletalk.aevt.inbox` and
# answer from `auto_reply`.
#
# System 7.5 on a IIci because Script Editor is the only application on the
# image that can originate an event aimed at another machine.  Program
# linking is switched on at runtime, as in the sibling tests.

TEST_NAME := Apple events — inbound from a guest
TEST_DESC := Boot System 7.5, enable program linking, point Script Editor at our host port, and assert on the event the guest sends us and the reply it gets back.

TEST_ROM := roms/iici-368cadfe.rom

TEST_SETUP := cp "$(TEST_DATA)/systems/system_7_5_0_77mb_mode32_24ac.img" "$(TEST_TMPDIR)/hd.img"

TEST_ARGS := model=iici ram=8192 hd=$(TEST_TMPDIR)/hd.img

# CI tier (proposal-integration-test-rework §5.4): unit | matrix | extended
TEST_TIER := extended
