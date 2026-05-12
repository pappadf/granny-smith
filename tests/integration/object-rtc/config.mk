# Integration test: rtc object class (M7b)
# Verifies rtc.time as a writable attribute (replacing set-time as the
# canonical entry per the M7 plan), and the rtc.pram child object's
# peek/poke/snapshot methods (honouring the write-protect bit).

TEST_NAME := Object-model RTC class
TEST_DESC := rtc.time / rtc.pram peek+poke+snapshot

TEST_ROM := roms/Plus_v3.rom
