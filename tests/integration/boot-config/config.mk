# Integration test: configuration-document boot (successor to
# nubus-staged-config).  Exercises machine.boot as an atomic, COMPLETE boot
# document: model/rom required, omitted fields resolve to the model's own
# defaults (never the previous record's, including across a model change),
# validate-before-teardown, the per-slot slots= configuration and its
# video_card= sugar, the machine.config record (including resolved vROM
# picks), the vrom= / slot rom= revision pin, and the checkpoint round-trip
# of the record.

TEST_NAME := Configuration-document boot (IIcx)
TEST_DESC := machine.boot document semantics, machine.config record, per-slot configuration

TEST_ROM := roms/iix-iicx-se30-97221136.rom
TEST_ARGS := model=iicx ram=8192

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
