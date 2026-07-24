# Integration test: Quadra 950 boots System 7.1 from SCSI HD and switches
# the display to Thousands (proposal-machine-quadra-700-900-950.md Phase H
# gate: "q950 boots; Monitors shows and switches to Thousands")
#
# The dedicated 3DC27823 ROM on the 33 MHz tower profile: model sense $90
# selects InfoQuadra950 (BoxFlag 20), the same Eclipse board path as the
# Q900 (Caboose + IOPs + dual 53C96), plus the Phase H video revision —
# DAFB 3 (DAFB_Test version bits read 3, the driver's 16bpp-always-allowed
# check) and the AC842a RAMDAC.  The script drives the real UI: PrimaryInit
# must pass the AC842a presence probe (PCBR1 behind AddrReg==1 without
# clobbering PCBR0) for Monitors to list "Thousands"; clicking it makes the
# driver write PCBR1=$C0 and the scanout switch to big-endian x555 16bpp.

TEST_NAME := Quadra 950 SCSI HD boot + Monitors switch to Thousands
TEST_DESC := 33 MHz tower; Thousands via Monitors UI + About box, both in x555

TEST_ROM := roms/q950-3dc27823.rom

# Copy the HD image into TEST_TMPDIR: the System writes to the volume during
# boot; the source image stays untouched between runs.
TEST_SETUP := cp "$(TEST_DATA)/systems/system_7_1_20mb_24ac_cd_32bit.img" "$(TEST_TMPDIR)/hd.img"

TEST_ARGS := model=q950 ram=8192 hd=$(TEST_TMPDIR)/hd.img
