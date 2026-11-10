-- Converted from test.script by scripts/script-to-lua.py
-- Cold boot of an installed Windows NT 4.0 on an Apple Network Server 500
-- with the PRODUCTION ROM -- Open Firmware 1.1.22, the one every Network
-- Server shipped with.  ans-nt-installed-boot is the same boot on the 2.26NT
-- ROM Apple built for NT; until 6 November 2026 that ROM was the only one
-- NT would run on.
--
-- WHY A ROW OF ITS OWN.  powermac-nt-hal's firmware (arcfw, an XCOFF the
-- ROM's own xcoff-loader starts) takes nothing from the 2.26NT ROM any
-- more: little-endian mode is a configuration variable on both ROMs
-- (setup.of), the floppy's scripts claim and map through /chosen's memory
-- and mmu instances instead of pe-loader's map-space, and the firmware walks
-- FAT and ISO 9660 itself and reads the disk raw, because this ROM's
-- fat-files stops one directory down and its partition instances open and
-- then answer 0 bytes to every read.  This row is what proves the NT ROM is
-- no longer required, and it is the boot a person with a real Network
-- Server and no ROM dump can make.
--
-- WHAT IT EXERCISES IN THE EMULATOR that the 2.26NT row does not: this
-- ROM's POST and Open Firmware running little-endian after `reset-all`
-- (relocated to real-base, MSR 0x1B071), its xcoff-loader reading
-- little-endian XCOFF headers, and the client interface's claim and map
-- with mode 0.  The rungs, the disk and the goldens are
-- ans-nt-installed-boot's (lib/nt.script, nt_installed_boot_rungs):
--
--   B1  the production firmware reaches `0 >` on ttya
--   B2  setup.of switches to little-endian and the machine comes back
--   B3  bootdisk.of starts arcfw, OSLOADER loads the kernel, NT runs on the HAL
--   B4  the logon screen
--   B5  Ctrl+Alt+Del, Return: Administrator is on the desktop
--
-- BUDGET.  6.9 G instructions to the desktop on 6 November 2026, against
-- 8.3 G on the 2.26NT ROM: this ROM's POST is the shorter.

local mac = dofile("../lib/mac.lua")
dofile("../lib/ans.lua")
dofile("../lib/nt.lua")

local nt_rom = "../../data/roms/ans500-ans700-962f6c13.rom"
local nt_disk = "../../data/systems/winnt_4_0_ppc_ans_512mb.img"
local nt_floppy = "../../data/systems/ans_nt_boot_floppy_1741579.img"

if mac.have_media(nt_rom) and mac.have_media(nt_disk) and mac.have_media(nt_floppy) then
    nt_installed_boot_rungs(nt_rom, nt_disk, nt_floppy, "ans-nt-stock-boot", "nt-logon.png", "nt-logon-information.png", "nt-desktop.png")
else
    mac.skip_test("ans-nt-stock-boot (the installed NT disk or the boot floppy is not in tests/data -- gs-test-data 30b6b8e or later; run scripts/fetch-test-data.sh)")
end

quit()
