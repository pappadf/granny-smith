# Integration test: a beige G3 restore that fails mid-construction leaves the
# running G3 intact.  Restore builds the new machine beside the running one;
# the new machine's ATA cells are wired before the stream runs out, and the
# failed build is destroyed.  The running machine's ATA interrupts, DMA kicks
# and dma_pump events must still name the running machine -- the cells'
# callback context is per machine -- so its boot continues, reads the ATA
# disk, and reaches the desktop with no system error.

TEST_NAME := Gossamer failed restore keeps the running G3
TEST_DESC := Beige G3: a truncated checkpoint fails to load mid-boot; the running machine keeps booting and doing ATA and SCSI I/O

TEST_ROM := roms/pmg3dt-pmg3mt-78f57389.rom

TEST_SETUP := cp "$(TEST_DATA)/systems/mac_os_9_2_1_516mb_g3.img" "$(TEST_TMPDIR)/hd.img"

TEST_RUNNER := run.sh

TEST_TIMEOUT := 3600

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := matrix
