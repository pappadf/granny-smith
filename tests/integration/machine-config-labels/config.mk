# Integration test: the labels of every model's machine-description tree.
#
# catalog.profile carries every label the configuration dialog shows, written
# by the core (docs/reference/object-model.md, "catalog.profile").  This walks
# every model's tree and fails on a label that breaks the labelling rules:
# internal identifiers (FD0, SLOT1_PCI0), a chip name standing as a label,
# "640x480" for "640 × 480", British spelling, emulator artefacts ("(generic",
# "on-board"), two things on one machine sharing a label, or a label not in
# sentence case.

TEST_NAME := Configuration labels
TEST_DESC := Every label in every model's catalog.profile tree follows the labelling rules (no internal ids, sentence case, unique)

# Any ROM works: catalog.profile is a registry lookup.
TEST_ROM := roms/plus-v3-4d1f8172.rom
TEST_RUNNER := run.sh

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
