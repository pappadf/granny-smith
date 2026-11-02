# Integration test configuration: a two-player Marathon 2 game across routers
#
# Two headless Power Macintosh 8100s, A and B, each on its own LocalTalk
# network (301, 302) with its own router, linked over UDP on the loopback
# interface -- the proposal's Appendix A row.  Both boot Mac OS 7.6.1 with
# Open Transport from gs-test-data's Marathon disk and start Marathon 2.
# B's player, "Carol", joins a network game; A's, "Alice", gathers one.
# Gather's NBP lookup reaches B's network as a FwdReq through both routers,
# B's guest answers across them, and A's Gather dialog lists Carol (the
# golden).  A adds Carol and starts the game: the map goes over ADSP and the
# game's ring of action packets runs over DDP, both ways through the
# routers, which the row checks on each side.
#
# The guests boot at turbo speed, then play at real time, as linked tabs do
# (Max is not offered while a Mac is linked).
#
# MEDIA-GATED: apps/marathon_12_pm8100.img (gs-test-data, fetched and
# extracted into tests/data/apps/); the row skips when it is missing.

TEST_NAME := AppleTalk Marathon across routers
TEST_DESC := Two 8100s on two routed networks: Gather finds the joined player through both routers, and the game's traffic runs both ways.

TEST_ROM := roms/pm6100-pm7100-pm8100-9feb69b3.rom

TEST_RUNNER := run.sh

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := extended
