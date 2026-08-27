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
#   <card-id, _ -> ->[-<rev>]-<crc8>.prom   e.g. mach64-gx-104-437584e0.prom
#
# The <targets>/<rev> parts are human facts (marketing revs, Apple part
# generations) that cannot be derived from the bytes, so the grammar reduces
# to a pure content-identity -> name table.  This module is the single owner
# of that mapping (proposal-content-addressed-rom-provisioning.md section 3.6b);
# its only consumers are scripts/rom-manifest.sh and the rom-naming
# conformance test.  The emulator never sees it.

# Content identity (8 lowercase hex digits: the stored checksum for CPU ROMs,
# the Format-Block CRC for vROMs, the whole-image CRC-32 for PCI expansion
# ROMs) -> canonical basename.
CANONICAL_NAMES = {
    # --- CPU ROMs (*.rom), keyed by stored checksum -------------------------
    "4d1f8172": "plus-v3-4d1f8172.rom",  # Macintosh Plus Rev 3 ("Loud Harmonicas")
    "97221136": "iix-iicx-se30-97221136.rom",  # Universal 256 KB IIx/IIcx/SE-30 ROM
    "4147dd77": "iifx-4147dd77.rom",  # Macintosh IIfx
    "368cadfe": "iici-368cadfe.rom",  # Macintosh IIci ("Aurora")
    "36b7fb6c": "iisi-36b7fb6c.rom",  # Macintosh IIsi ("Erickson")
    "420dbff3": "q700-q900-420dbff3.rom",  # Quadra 700/900 (also PB140/170)
    "3dc27823": "q950-3dc27823.rom",  # Quadra 950
    "5bf10fd1": "q840av-q660av-5bf10fd1.rom",  # Quadra 840AV / Centris 660AV (Cyclone/Tempest)
    "9feb69b3": "pm6100-pm7100-pm8100-9feb69b3.rom",  # Power Macintosh 6100/7100/8100 (1994-03 PDM ROM)
    "96cd923d": "pm7500-pm8500-pm9500-96cd923d.rom",  # Power Macintosh 7500/8500/9500 (1995-08 TNT ROM v1)
    "9630c68b": "pm7500-pm8500-pm9500-v2-9630c68b.rom",  # Power Macintosh 7500/8500/9500 (1995-08 TNT ROM v2)
    # The Apple Network Servers.  The production image carries the SAME
    # version string as the 9500 v2 ROM above, so only the checksum tells
    # them apart (proposal-apple-network-server-500-700 §2.3).
    "962f6c13": "ans500-ans700-962f6c13.rom",  # Apple Network Server 500/700 (Open Firmware 1.1.22, AIX)
    "49b2be8f": "ans500-ans700-proto20-49b2be8f.rom",  # Apple Network Server 500/700 (2.0 prototype, Mac OS)
    "098917b2": "lisa2-revh-098917b2.rom",  # Apple Lisa 2 boot ROM rev H (computed checksum)
    "094c82f0": "macxl-3a-094c82f0.rom",  # Macintosh XL boot ROM "3A" (computed checksum)
    # --- Declaration ROMs / vROMs (*.vrom), keyed by Format-Block CRC -------
    "d1629664": "mdc-8-24-revb-d1629664.vrom",  # Display Card 8-24 (JMFB), part 341-0868 Rev B
    "4f71ff1a": "builtin-se30-video-4f71ff1a.vrom",  # SE/30 onboard video
    "d8daab87": "display-card-24ac-d8daab87.vrom",  # Display Card 24AC
    "d722b053": "824gc-v1.1-revb-d722b053.vrom",  # 8-24 GC v1.1, part 341-0266 (16bpp default)
    "9e9857e8": "824gc-v1.0-reva-9e9857e8.vrom",  # 8-24 GC v1.0 shipping, part 341-0812-02
    "4740028d": "824gc-v1.0a16-4740028d.vrom",  # 8-24 GC 1.00a16 alpha ("Dolphin")
    # --- PCI expansion ROMs (*.prom), keyed by whole-image CRC-32 -----------
    "437584e0": "mach64-gx-104-437584e0.prom",  # Apple Accelerated PCI Graphics Card, ROM 113-32900-104
    "8c68216e": "mach64-gx-101-8c68216e.prom",  # ...and the earlier -101 programming
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
