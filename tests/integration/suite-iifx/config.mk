# Integration suite: Macintosh IIfx (proposal-integration-test-rework §7)
#
# One daemon run; rows re-instantiate via machine.boot and share the
# harness in ../lib/mac.script. Absorbs iifx-boot, iifx-boot-chime,
# iifx-24ac, iifx-824gc (re-media'd to 7.1 per §7) and iifx-aux3-boot.
#
# The IIfx has no built-in video: every row names its NuBus card
# explicitly, both because the slot-$9 default (mdc_8_24) is only a
# default and because a row must never inherit the card a previous row
# seated.
#
# RAM axis (options 4/8/16/32/64/128 MB): the rows spread 8/16/32 MB.
# 8 MB is deliberately only on the ROM-only chime row and on the
# iifx-8m MILESTONE row, because booting the IIfx at 8 MB is a known
# emulator defect — iifx-boot/config.mk used to pin 16 MB with the note
# "ram=8192 currently regresses the boot (stalls in POST at $40843F96)",
# tracked by nothing. §7 turns that into a tracked, run-not-fatal row.
#
# NOT covered here: the paired 24-bit/32-bit rows §3.4 wants. The
# addressing mode is PRAM-selected on a clean-ROM machine, but the
# selector byte is still unidentified — measured 2026-07-28, seeding
# MMFlags (PRAM $8A, pram.md §4.3) to $80/$00 leaves the IIfx booting
# 7.0.1 in 24-bit either way, so that byte is ruled out. The declared
# 32-bit IIfx cell stays in matrix-targets.json marked `blocked` so the
# debt keeps reporting; 32-bit coverage meanwhile comes from the IIci and
# Q950 7.6 rows, where the System selects it itself.
#
#   make test-suite-iifx
#   make test-suite-iifx TEST_VARS="ROW=iifx-chime"   one row only
#   make test-suite-iifx TEST_VARS="REGEN=1"          recapture goldens

TEST_NAME := IIfx suite
TEST_DESC := Chime WAV, 6.0.8 IOP floppy, 7.0.1 JMFB + lowmem + About, 7.1 GC card, 24AC seat + 832x624 on 7.5, A/UX login gate

TEST_ROM := roms/iifx-4147dd77.rom
TEST_ARGS := model=iifx ram=16384

# CI tier (proposal-integration-test-rework §5.4): unit | matrix | extended
TEST_TIER := matrix
