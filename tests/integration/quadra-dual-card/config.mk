# Integration test: Quadra 700 with TWO NuBus display cards under System 7.1
# (proposal-integration-test-rework §2.2 / §7 merge pair)
#
# Replaces q700-24ac and q700-824gc, which were the SAME 1.5 B-cycle boot
# of the same image with the same golden, differing only in which card was
# seated and one card-specific assert. §2.2's conclusion: one dual-card
# boot covers both for half the cost — and additionally tests the
# multi-card path under a real OS, which nothing did before (iicx-dual-
# display proves the ROM-level contract only).
#
# Both cards go in explicitly via the per-slot staging channel
# (nubus.slot[N].card_id, the iicx-dual-display mechanism) rather than the
# video_card= wildcard, which can only seat one.
#
# The Quadras' DAFB must remain the boot screen with either card present
# (ScrnBase on the DAFB aperture), and on a 68040 the 8•24 GC's INIT must
# decline to start the coprocessor — both were the point of the originals.

TEST_NAME := Quadra 700 dual NuBus display cards
TEST_DESC := 24AC + 8*24 GC seated together under 7.6; DAFB keeps the boot screen, GC declines on 040

TEST_ROM := roms/q700-q900-420dbff3.rom

# Media is 7.6 per §7 (the dual-card contract is version-incidental, so the
# row buys the otherwise-dark Q700 x 7.6 cell). Copied into WORK_DIR because
# the System writes to the volume during boot.
TEST_SETUP := cp "$(TEST_DATA)/systems/system_7_6_170mb_24ac.img" "$(WORK_DIR)/hd.img"

TEST_ARGS := model=q700 ram=8192

# CI tier (proposal-integration-test-rework §5.4): unit | matrix | extended
TEST_TIER := extended
