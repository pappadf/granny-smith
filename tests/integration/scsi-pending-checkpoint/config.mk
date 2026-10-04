# Integration test: a checkpoint taken with a SCSI event pending restores
#
# A Quadra 700 with no disk probes every SCSI id as it starts; each probe
# arms the bus's selection timeout, a scheduler event.  The test saves at an
# instant one is pending and loads the checkpoint: the event types a bus
# uses are registered when the bus is built, so the restored queue binds it.
# (They were registered on first use, so a checkpoint taken mid-probe named
# a type the new machine did not have yet, and the load failed.)

TEST_NAME := SCSI event pending at a checkpoint
TEST_DESC := Quadra 700 saved with the selection timeout pending, then restored

TEST_ROM := roms/q700-q900-420dbff3.rom

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
