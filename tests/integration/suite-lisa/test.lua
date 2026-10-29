-- Converted from test.script by scripts/script-to-lua.py
-- Lisa 2 / Macintosh XL suite (see config.mk).
--
-- Row map (former directory → row):
--
--   lisa-los31-profile    lisa-profile-boot: cold-boot LOS 3.1 from the
--                         installed ProFile, driven by the image's own
--                         on-disk PRAM snapshot
--   lisa-los31-lisaem     the same boot from a LisaEm ProFile image (a
--                         DiskCopy 4.2 file with 20-byte tags), attached
--                         as it is
--   lisa-xenix-hdinit     lisa-xenix-hdinit: Xenix 3.0 hard-disk init
--                         from the boot floppy (the fast-tier gate for
--                         the full install, which stays in extended)
--   lisa-xenix-boot       lisa-xenix-boot: boot Xenix from the floppy
--                         with the installed ProFile attached
--   lisa-xenix-nofloppy   lisa-xenix-nofloppy: same machine with no
--                         floppy — the ROM's startup-from prompt path
--   xl-macworks-boot      xl-boot: MacWorks XL 3.0 loader ejects, the
--                         system disk goes in, the Finder comes up
--   xl-no-media           xl-no-media: no boot device — the XL's
--                         DRVERR prompt rather than a clamp error
--
-- Every row keeps its original assertions and choreography verbatim; see
-- rows.script.

local mac = dofile("../lib/mac.lua")
dofile("rows.lua")

mac.row("lisa-los31-profile", row_lisa_los31_profile)
if mac.row_on("lisa-los31-lisaem") then
    if mac.have_media("../../data/Lisa/LisaOfficeSystem-3.1/LOS-3.1-apps.dc42") then
        mac.row("lisa-los31-lisaem", row_lisa_los31_lisaem)
    else
        echo("skip: lisa-los31-lisaem (no LisaEm ProFile image in the test data)")
    end
end
mac.row("lisa-xenix-hdinit", row_lisa_xenix_hdinit)
mac.row("lisa-xenix-boot", row_lisa_xenix_boot)
mac.row("lisa-xenix-nofloppy", row_lisa_xenix_nofloppy)
mac.row("xl-macworks-boot", row_xl_macworks_boot)
mac.row("xl-no-media", row_xl_no_media)
mac.row("lisa-checkpoint", row_lisa_checkpoint)

mac.suite_done()
quit()
