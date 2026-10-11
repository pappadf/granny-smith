# Integration suite: part of the AV family suite, run as its own test so
# no single test sets the CI shard floor.  test.script names the rows
# (av-beep, av-tts, av-sound-in, av-sr-command, av-sr-macro) and includes ../suite-av/test.script, whose header describes
# them; goldens/ links to that directory's goldens.
#
#   make test-suite-av-sound
#   make test-suite-av-sound TEST_VARS="ROW=av-beep"     one row only
#   make test-suite-av-sound TEST_VARS="REGEN=1"      recapture goldens

TEST_NAME := AV suite: sound and speech rows
TEST_DESC := SysBeep, text-to-speech, sound input and speech recognition through the DSP

TEST_ROM := roms/q840av-q660av-5bf10fd1.rom
TEST_ARGS := model=q840av ram=16384

TEST_TIMEOUT := 1200

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := matrix
