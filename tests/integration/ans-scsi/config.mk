# Integration test: the SCRIPTS engine and both fast/wide SCSI channels
# (proposal-apple-network-server-500-700 §5.7, ladder rung S8)
#
# The 53C8xx is a new DEVICE CLASS for this repository, not a variation on
# anything already here.  Every other SCSI controller in the tree — the
# 5380, the 53C94/96, MESH — is register-driven: a driver writes a command
# byte and polls.  A 53C825A fetches and executes an instruction set out of
# HOST memory, and a register-level model that does not run SCRIPTS moves
# exactly zero bytes.
#
# This row drives it the way the machine does: Open Firmware's own
# `probe-scsi1` and `probe-scsi2`, which build a SCRIPT in RAM, hand its
# address to DSP, and read back what the targets said.  Note the words are
# `probe-scsi1` / `probe-scsi2` — this machine has its own, one per
# controller, and `probe-scsi` / `probe-scsi-all` do not exist on it, so a
# row that typed those would prove nothing.
#
# THE MACHINE HAS THREE SCSI BUSES and the two fast/wide ones are separate
# namespaces: `machine.scsi` is channel 0 (bays 0-3, the `disk0`..`disk3`
# aliases) and `machine.scsi2` is channel 1 (bays 4-6, plus the 700's two
# rear drives).  This is the first machine in the repository where a SCSI
# id does not identify a device on its own.

TEST_NAME := ANS fast/wide SCSI
TEST_DESC := Runs the SCRIPTS engine through Open Firmware's probe-scsi1 and probe-scsi2 on both 53C825A channels

# 4 MB Apple Network Server 500/700 ROM, Open Firmware 1.1.22.
TEST_ROM := roms/ans500-ans700-962f6c13.rom

TEST_ARGS := model=ans500 ram=32768

# CI tier (proposal-integration-test-rework §5.4): unit | matrix | extended
TEST_TIER := matrix
