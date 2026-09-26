# Integration test: configuration-document boot (successor to
# nubus-staged-config).  Exercises machine.boot as an atomic, COMPLETE boot
# document: model/rom required, omitted fields resolve to the model's own
# defaults (never the previous record's, including across a model change),
# validate-before-teardown, the wildcard video_card= argument, the surviving
# per-slot staged surface, the machine.config record (including resolved
# vROM picks), the explicit vrom= revision pin, machine.restart, and the
# checkpoint round-trip of the record.

TEST_NAME := Configuration-document boot (IIcx)
TEST_DESC := machine.boot document semantics, machine.config record, per-slot staging

TEST_ROM := roms/iix-iicx-se30-97221136.rom
TEST_ARGS := model=iicx ram=8192

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
