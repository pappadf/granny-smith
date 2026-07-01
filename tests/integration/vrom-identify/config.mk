# Integration test: vrom.identify probe surface (NuBus declaration-ROM
# Format-Block CRC identity).
#
# Mirrors rom-identify: cross-checks the C-side VROM_CATALOG against what the
# web2 machine-config dialog expects.  Asserts the {card_id, compatible, crc}
# contract for each shipped vROM (keyed off the genuine Format-Block CRC, the
# analog of rom.c's checksum), and that every catalog card_id resolves to a
# registered nubus card kind.  Boots the Universal ROM as IIcx so the nubus
# object (and its video_card setter) is present.

TEST_NAME := vrom.identify probe surface
TEST_DESC := vrom.identify Format-Block CRC identity + card_id/compatible contract + catalog/registry drift guard

TEST_ROM := roms/IIcx.rom

TEST_ARGS := model=iicx ram=8192
