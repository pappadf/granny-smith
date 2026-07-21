# Integration test: sound object class (M7f)
# Plus PWM sound is the test target — SE/30 / IIcx use ASC and leave
# cfg->sound NULL, so `sound.*` only resolves on Plus.

TEST_NAME := Object-model sound class
TEST_DESC := sound.enabled / .volume / .sample_rate / .mute() — Plus PWM module

TEST_ROM := roms/plus-v3-4d1f8172.rom
