# Integration test configuration: AppleTalk routers linked over UDP
#
# Three headless instances, each its own LocalTalk network with its own
# router (appletalk_router.c), linked in a triangle over UDP on the loopback
# interface (appletalk.router.link_udp).  The routers exchange RTMP and ZIP:
# every network and zone becomes known everywhere, at distance 1.  Then C
# closes its link to A, and A reroutes to C's network through B (distance 2),
# as RTMP's notify-neighbor and Update-the-Entry rules say it must.  The
# machines need no disk: the routers run while the machine does.

TEST_NAME := AppleTalk router links
TEST_DESC := Three routers in a triangle over UDP learn every network and zone; a cut link reroutes through the third.

TEST_ROM := roms/plus-v3-4d1f8172.rom

TEST_RUNNER := run.sh

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
