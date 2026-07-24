# Integration test: Quadra 900 boots System 7.1 from SCSI HD to the desktop
# (proposal-machine-quadra-700-900-950.md Phase G gate)
#
# The tower path over the shared 420DBFF3 ROM: model sense $D0 selects the
# Eclipse ProductInfo (ClockEgret + Caboose, ADBIop, dual 53C96, IOPs), so
# this boot exercises the whole Phase G device set — Caboose RTC/PRAM over
# the VIA1 Egret handshake, the SCC/SWIM IOP firmware upload + mailbox
# protocol (ADB autopoll through the SWIM IOP), and the internal 53C96.
# The ROM uploads the same IOP firmware images as the IIfx (both hashes
# verify), so the IIfx behavioural models are exercised end-to-end here.

TEST_NAME := Quadra 900 System 7.1 SCSI HD boot to desktop
TEST_DESC := Tower boot via Caboose + IOPs + dual 53C96: desktop pixel-match

TEST_ROM := roms/q700-q900-420dbff3.rom

# Copy the HD image into TEST_TMPDIR: the System writes to the volume during
# boot (desktop DB etc.); the source image stays untouched between runs.
TEST_SETUP := cp "$(TEST_DATA)/systems/system_7_1_20mb_24ac_cd_32bit.img" "$(TEST_TMPDIR)/hd.img"

TEST_ARGS := model=q900 ram=8192 hd=$(TEST_TMPDIR)/hd.img
