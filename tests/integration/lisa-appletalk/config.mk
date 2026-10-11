# Integration test configuration: Apple Lisa 2 — AppleTalk on Serial B.
# Boots the Lisa 2 (rev-H boot ROM) to its idle state, then runs a small
# routine that puts SCC channel B in SDLC mode and sends an LLAP lapENQ to
# the emulated network's own node, which answers with a lapACK: the frame
# reaches the network from the Lisa's SCC, and the reply comes back to it.

TEST_NAME := Apple Lisa 2 AppleTalk
TEST_DESC := LLAP ENQ/ACK between the Lisa's SCC channel B and the emulated AppleTalk network

# Interleaved rev-H Lisa 2 boot ROM (341-0175-H / 341-0176-H), 16 KB, checksum 0x098917B2.
TEST_ROM := roms/lisa2-revh-098917b2.rom

# Lisa 2, 1 MB.  No disks: the routine drives the SCC directly.
TEST_ARGS := model=lisa ram=1024

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
