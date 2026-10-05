# Integration test: the PRAM a machine is built with (rtc.h pram_defaults_t)
# Every model with an RTC is constructed and its PRAM read before a single
# instruction runs: the ROM family's XPRAM token, its Start Manager table,
# its own cold MMFlags (plus bit 5 on the PDM family), and the Start
# Manager's no-wait bit.  SysParam stays invalid for the ROM to initialise.

TEST_NAME := PRAM at construction
TEST_DESC := each ROM family's power-up PRAM, read at instruction 0

# The script boots every model itself; the CLI ROM is only the first one.
TEST_ROM := roms/plus-v3-4d1f8172.rom

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
