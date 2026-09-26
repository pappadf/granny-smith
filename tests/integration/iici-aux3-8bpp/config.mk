# Integration test configuration: IIci A/UX 3.0.1 HD boot at 8 bpp, on the
# built-in RBV video (MILESTONE class: run, reported, not fatal).
#
# RBV-under-A/UX is a video path nothing else exercises.  The IIci rather
# than the IIsi because A/UX requires an FPU and the IIci has a 68882.  The
# row seeds the RBV to 8 bpp through its slot-PRAM record, as suite-iici's
# iici-701-depths does (PRAM $56, BoardID $001F, depth spID $83; see
# test.script), and expects the graphical login window at 8 bpp.
#
# It does not get there today: the guest draws at 8 bpp while the RBV scans
# out, and machine.screen reports, 1 bpp (#183).  Under System 7.0.1 the same
# seed gives a correct 8-bpp desktop, so the defect is on the A/UX path.  The
# row reproduces it nightly; promotion to enforced is a reviewed edit here
# plus removing the cell's blocked marker in matrix-targets.json.
#
# The 8-bpp configuration also exposed two IIfx defects when this test ran on
# that machine, both fixed: the JMFB's PRAM seeding (the token stamp once
# suppressed the ROM's startup-device init, so SCSILoad found no boot
# driver), and PMMU instruction-fetch faults routed through the non-retry bus
# error path (a false double-fault HALT when A/UX demand-paged /etc/init).

TEST_NAME := IIci A/UX 3.0.1 HD Boot at 8 bpp (reaches graphical login)
TEST_DESC := Boot IIci (16 MB, built-in RBV at 8 bpp) from the A/UX 3.0.1 HD image; expect the graphical login window, pixel-exact.

# IIci ROM (checksum 0x368CADFE).  Video is the machine's built-in RBV, not a
# NuBus card, so no declrom is discovered or needed — that is the point of the
# re-host.
TEST_ROM := roms/iici-368cadfe.rom

TEST_ARGS := model=iici ram=16384

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := extended
