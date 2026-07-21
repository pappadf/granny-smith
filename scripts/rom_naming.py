#!/usr/bin/env python3
# rom_naming.py - Canonical fixture-filename grammar for the gs-test-data repo.
#
# The emulator core identifies every ROM/vROM purely by content (checksum /
# NuBus Format-Block CRC) and knows no filenames.  Fixture files in
# gs-test-data's roms/ directory, however, are browsed by humans, so they
# keep the legible canonical grammar from proposal-test-rom-naming.md:
#
#   <targets>[-<rev>]-<checksum8>.rom       e.g. iix-iicx-se30-97221136.rom
#   <card-id, _ -> ->[-<rev>]-<crc8>.vrom   e.g. mdc-8-24-revb-d1629664.vrom
#
# The <targets>/<rev> parts are human facts (marketing revs, Apple part
# generations) that cannot be derived from the bytes, so the grammar reduces
# to a pure content-identity -> name table.  This module is the single owner
# of that mapping (proposal-content-addressed-rom-provisioning.md section 3.6b);
# its only consumers are scripts/rom-manifest.sh and the rom-naming
# conformance test.  The emulator never sees it.

# Content identity (8 lowercase hex digits: the stored checksum for CPU ROMs,
# the Format-Block CRC for vROMs) -> canonical basename.
CANONICAL_NAMES = {
    # --- CPU ROMs (*.rom), keyed by stored checksum -------------------------
    "4d1f8172": "plus-v3-4d1f8172.rom",  # Macintosh Plus Rev 3 ("Loud Harmonicas")
    "97221136": "iix-iicx-se30-97221136.rom",  # Universal 256 KB IIx/IIcx/SE-30 ROM
    "4147dd77": "iifx-4147dd77.rom",  # Macintosh IIfx
    "368cadfe": "iici-368cadfe.rom",  # Macintosh IIci ("Aurora")
    "36b7fb6c": "iisi-36b7fb6c.rom",  # Macintosh IIsi ("Erickson")
    "098917b2": "lisa2-revh-098917b2.rom",  # Apple Lisa 2 boot ROM rev H (computed checksum)
    "094c82f0": "macxl-3a-094c82f0.rom",  # Macintosh XL boot ROM "3A" (computed checksum)
    # --- Declaration ROMs / vROMs (*.vrom), keyed by Format-Block CRC -------
    "d1629664": "mdc-8-24-revb-d1629664.vrom",  # Display Card 8-24 (JMFB), part 341-0868 Rev B
    "4f71ff1a": "builtin-se30-video-4f71ff1a.vrom",  # SE/30 onboard video
    "d8daab87": "display-card-24ac-d8daab87.vrom",  # Display Card 24AC
    "d722b053": "824gc-v1.1-revb-d722b053.vrom",  # 8-24 GC v1.1, part 341-0266 (16bpp default)
    "9e9857e8": "824gc-v1.0-reva-9e9857e8.vrom",  # 8-24 GC v1.0 shipping, part 341-0812-02
    "4740028d": "824gc-v1.0a16-4740028d.vrom",  # 8-24 GC 1.00a16 alpha ("Dolphin")
}


def canonical_name(content_id):
    """Map a content identity to its canonical fixture basename, or None.

    Accepts the identity in any of the forms the identify surfaces emit:
    "0xd1629664", "d1629664", or "D1629664".
    """
    if not content_id:
        return None
    ident = content_id.strip().lower()
    if ident.startswith("0x"):
        ident = ident[2:]
    return CANONICAL_NAMES.get(ident)
