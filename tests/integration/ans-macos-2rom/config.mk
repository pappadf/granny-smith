# Integration test: the Network Server's 2.0 prototype ROM, running Mac OS
# (proposal-apple-network-server-500-700 §5.1, ladder rung S14)
#
# THE CROSS-EXAMINATION.  The Apple Network Servers shipped a ROM with no
# Mac OS Toolbox at all: the production image ($962F6C13) boots AIX and
# refuses Mac OS, and there is no image that does both.  What survives is a
# 2.0 PROTOTYPE ($49B2BE8F) that is the mirror image — Mac OS only, no AIX —
# and it drives the same 54M30 and the same two 53C825As.
#
# That pairing is unusually good evidence, and it is the whole reason this
# row exists.  Two entirely unrelated software stacks exercise one hardware
# model: anything the model gets wrong that both stacks tolerate is
# genuinely invisible, and anything only one of them tolerates shows up here
# rather than in a bug report.  It is also the fallback named in the
# proposal's R4 — when AIX misbehaves and there is no source to disassemble,
# a device can be cross-examined through a stack we do understand.
#
# The prototype ROM misconfigures the L2 cache and was never shipped; it is
# a research artifact, not a supported configuration.

TEST_NAME := ANS Mac OS cross-check
TEST_DESC := Boots the 2.0 prototype ROM on the same Network Server hardware model and asserts it takes the Mac OS path

# 4 MB Apple Network Server 2.0 prototype ROM (stored checksum 0x49B2BE8F),
# reassembled from four per-lane chip dumps; the reassembly self-verifies
# the Apple checksum over the image's first 3 MiB.
TEST_ROM := roms/ans500-ans700-proto20-49b2be8f.rom

TEST_ARGS := model=ans500 ram=32768

# CI tier (proposal-integration-test-rework §5.4): unit | matrix | extended
TEST_TIER := matrix
