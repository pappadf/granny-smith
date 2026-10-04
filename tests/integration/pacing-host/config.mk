# Integration test: the pacing setting is the host's.
#
# The toolbar, ?speed= and --speed= choose how fast the emulator runs.  That
# choice used to be reachable only as scheduler.mode, which needs a machine, so
# a page could not set it before it booted one and ?speed= stayed UI state.
# `pacing` is the setting itself, there with or without a machine: every
# machine booted or restored runs under it, and scheduler.mode is the same
# setting reached through the running machine.

TEST_NAME := Host pacing
TEST_DESC := pacing.* is the host's setting; booted and restored machines run under it; scheduler.mode is the same setting

TEST_ROM := roms/plus-v3-4d1f8172.rom

TEST_ARGS := model=plus

TEST_TIER := unit
