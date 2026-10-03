# Integration test: machine.cpu.mmu's inspection methods on every MMU kind.
# translate, walk, map and descriptor answer the same shapes on the 68030
# PMMU, the 68040, the PowerPC 601 and 604 and the Lisa's segment MMU; this
# row boots one machine of each and checks that the four agree with each
# other on live guest tables.

TEST_NAME := machine.cpu.mmu translate / walk / map / descriptor
TEST_DESC := walk agrees with translate, map runs translate linearly, descriptors match the walk, on 030/040/601/604/Lisa

TEST_ROM := roms/plus-v3-4d1f8172.rom

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
