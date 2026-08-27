# Integration test: Apple Network Server ROM boot ladder
# (proposal-apple-network-server-500-700 §8.1)
#
# THE verification instrument for the ANS bring-up.  Boots Apple's
# production Open Firmware 1.1.22 ROM headless for a bounded instruction
# budget and asserts every ladder marker up to the current high-water rung,
# on BOTH profiles.  The expected rung is committed alongside the code that
# reaches it; any change that drops a rung fails this row with the name of
# the first missing marker.
#
# WHAT MAKES THIS ROW UNUSUAL.  Apple published the exact strings POST
# writes to the front-panel LCD — the progress sequence, the banner, and
# every failure message — so rungs S2 through S5 assert on DOCUMENTED TEXT,
# positively (the expected progress message appeared) AND negatively (no
# `MainLBU 825#1 Failed`, no `Video ID Bad`).  No previous machine in this
# repository has offered anything like it, and it is why the LCD is built
# before anything else: POST establishes its LCD path before it sizes DRAM,
# so the panel is the machine's only narrator during exactly the phase most
# likely to break.
#
# BOTH PROFILES, EVERY TIME.  `ans500` and `ans700` differ only in the CPU
# card's clock, the L2 DIMM size and `TwoSuppliesH`, which is precisely the
# situation where a bug hides in the model nobody tested (proposal §13 R10).

TEST_NAME := ANS ROM ladder
TEST_DESC := Boots the Apple Network Server ROM on ans500 and ans700 and asserts the §8.1 ladder markers up to the committed high-water rung

# 4 MB Apple Network Server 500/700 ROM, Open Firmware 1.1.22
# (stored checksum 0x962F6C13, spanning 3 MiB of the 4 MiB image).
TEST_ROM := roms/ans500-ans700-962f6c13.rom

TEST_ARGS := model=ans500 ram=32768

# CI tier (proposal-integration-test-rework §5.4): unit | matrix | extended
TEST_TIER := unit
