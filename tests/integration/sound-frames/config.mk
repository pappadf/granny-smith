# Integration test: machine.sound.frames counts sample frames on every engine.
#
# The sound surface documents `frames` as "output frames rendered since
# power-on".  The Plus PWM scan, the ASC and the DAVbus engines count sample
# frames; the PDM AWACS engine used to count half-buffers and the AV Singer
# its frame-engine ticks (incremented even with the output engine off), so
# the same attribute meant three different units.  Each check starts a
# capture at power-on, plays the startup sound and asserts that the frame
# count equals the frames the capture sink recorded.

TEST_NAME := Sound frame count units
TEST_DESC := machine.sound.frames == machine.sound.capture.frames after the startup sound (Plus, PDM, AV)

TEST_ROM := roms/plus-v3-4d1f8172.rom
TEST_ARGS := model=plus

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
