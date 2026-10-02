# Integration test: the TNT's NVRAM follows the machine, not the process.
#
# The #112 regression row.  The 8 KB store used to ride a process-lifetime
# holder across every teardown, so a machine.boot after a run cut short
# part-way through Open Firmware's format of a virgin store inherited a torn
# store and never booted (Open Firmware sat at its serial-console prompt
# behind a black screen).  Now the store survives exactly the operations that
# keep the machine -- machine.reset and machine.restart -- and every new
# machine (machine.boot, machine.rebuild) starts from a virgin one.

TEST_NAME := TNT NVRAM lifetime (#112)
TEST_DESC := NVRAM survives machine.reset and machine.restart; machine.boot and machine.rebuild start blank; a boot after a torn store still boots

TEST_ROM := roms/pm7500-pm8500-pm9500-96cd923d.rom
TEST_ARGS := model=pm7500 ram=32768

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := matrix
