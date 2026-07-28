# Integration suite: Macintosh SE/30 (proposal-integration-test-rework §7)
#
# One daemon run; rows re-instantiate via machine.boot and share the
# harness in ../lib/mac.script. Absorbs se30-boot (no-media icon),
# se30-boot-chime, se30-floppy-hd (re-media'd to real 7.1 Disk Tools),
# se30-gsvrom, and boot-matrix's SE/30 6.0.8 row (which is the
# direct-SWIM transport row); adds an SE/30 x 7.5 HD row that also
# carries the SCSI-HD-boot coverage se30-scsi used to provide.
#
# Retired here, not lost: se30-floppy claimed SE/30 x 7.1 from
# System_7_1_0.dsk, an 800K disk that actually boots a 6.0.7-class
# system (§7's third media fiction) — the genuine 800K GCR transport
# coverage is the 6.0.8 row below. se30-scsi is deleted; its SCSI HD
# boot path is the 7.5 row, on fresher media.
#
#   make test-suite-se30
#   make test-suite-se30 TEST_VARS="ROW=se30-chime"    one row only
#   make test-suite-se30 TEST_VARS="REGEN=1"           recapture goldens

TEST_NAME := SE/30 suite
TEST_DESC := No-media icon, chime WAV, 6.0.8/7.1 floppy transports, generic vROM, 7.5 HD, checkpoint

TEST_ROM := roms/iix-iicx-se30-97221136.rom
TEST_ARGS := model=se30 ram=8192

# CI tier (proposal-integration-test-rework §5.4): unit | matrix | extended
TEST_TIER := matrix
