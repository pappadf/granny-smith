# Integration test: via1 / via2 object classes (M7c)
# Boots SE/30 (Universal ROM) so both VIAs are populated. Plus has
# only via1 — the SE/30 ROM exercises via2 alongside it via the
# RBV / SCSI handshake paths, so reads through `via2.*` actually see
# moving state by the time the script runs.

TEST_NAME := Object-model VIA classes
TEST_DESC := via1 / via2 status registers + port_a / port_b children

# Universal ROM shared by SE/30, IIcx, IIx (checksum 0x97221136)
TEST_ROM := roms/IIcx.rom
