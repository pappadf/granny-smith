# Integration test: configuration-document boot (successor to
# nubus-staged-config).  Exercises machine.boot as an atomic, COMPLETE boot
# document: model/rom required, omitted fields resolve to the model's own
# defaults (never the previous machine's, including across a model change),
# validate-before-teardown, the per-slot slots= configuration and its
# video_card= sugar, the declaration ROM each card resolved, the vrom= /
# slot rom= revision pin, and a checkpoint restoring the cards and the
# monitor strap the machine was built with.

TEST_NAME := Configuration-document boot (IIcx)
TEST_DESC := machine.boot document semantics, the cards and ROMs each slot seats, per-slot configuration

TEST_ROM := roms/iix-iicx-se30-97221136.rom
TEST_ARGS := model=iicx ram=8192

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
