# Integration test: rtc object class (M7b)
# Verifies rtc.time as a writable attribute (replacing set-time as the
# canonical entry per the M7 plan), pram read/write methods honoring
# the write-protect bit, and the read-only V_BYTES snapshot.

TEST_NAME := Object-model RTC class
TEST_DESC := rtc.time / rtc.pram / pram_read / pram_write

TEST_ROM := roms/Plus_v3.rom
