# Integration test: per-slot staged NuBus configuration (stage 2).
#
# The object-model config surface of
# proposal-nubus-computed-card-compatibility.md §5.6: empty sockets exist as
# slot nodes with staged card_id / video_mode attributes; concrete-slot
# staging beats the machine.nubus.video_card wildcard alias; everything is
# consumed exactly once at machine.boot.  Structural assertions only — no
# instruction budget beyond boot.

TEST_NAME := NuBus per-slot staged config
TEST_DESC := slot[N].card_id / video_mode staging, wildcard-alias equivalence, consume-at-boot

TEST_ROM := roms/iix-iicx-se30-97221136.rom

TEST_ARGS := model=iicx ram=8192
