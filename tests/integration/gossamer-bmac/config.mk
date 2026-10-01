# Integration test: the beige G3's BMAC Ethernet cell under Open Firmware's
# own network package — the station address from the cell's serial EEPROM,
# receive and transmit through DBDMA channels 3 and 2, the cell's
# loopback, and the firmware's BOOTP request on `boot enet`.

TEST_NAME := Gossamer BMAC
TEST_DESC := Open Firmware's BMAC package: EEPROM station address, DBDMA receive/transmit, loopback, BOOTP

TEST_ROM := roms/pmg3dt-pmg3mt-78f57389.rom

TEST_ARGS := model=pmg3dt ram=65536

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := matrix
