# Integration test: a recording is the picture on the monitor.
#
# The IIcx's Macintosh Display Card 8•24 on the 21" Kong monitor (sense $0,
# 1152x870, slot PRAM seeded for 8 bpp -- the iicx-video-modes recipe): the
# JMFB driver pre-distorts every CLUT write for the monitor's gamma, and the
# monitor's response curve (display_t.crt_response) takes it back out.  The
# recorder applies the curve, so a decoded frame equals machine.screen.save
# passed through crt_response (recorder.snapshot) and differs from
# screen.save itself, which stays the bus's bytes.  See
# docs/internals/core/record/record.md.

TEST_NAME := Recorder — monitor response (Kong 21")
TEST_DESC := A decoded frame equals screen.save through the Kong monitor's response curve, and not screen.save itself

TEST_ROM := roms/iix-iicx-se30-97221136.rom
TEST_ARGS := model=iicx ram=8192

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := matrix
