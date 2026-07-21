# Integration test configuration: IIcx + Display Card 8•24 GC — ADB mouse
# DELTA path (the web2 pointer-lock input route).
#
# Guards two bugs that froze the cursor in web2 while clicks kept working:
#   1. adb.c: an aborted SRQ-scan Talk R0 re-marked the mouse pending but the
#      deltas were already consumed into the reply buffer — the re-poll
#      reported zero movement (the keyboard got its savepoint fix in
#      4962aa4; the mouse never did, and every test drove the cursor via
#      the "global" low-memory writes that bypass ADB entirely).
#   2. display_card_824gc.c: the decl-ROM driver enables the slot VBL with a
#      12-bit SERIAL command (1 = run) bit-banged into the Am29000 latch at
#      $04C00000 — not via the JMFB Stopwatch registers.  Without it the
#      slot VBL never fired, so the cursor task (MTemp -> RawMouse/Mouse
#      scaling + redraw) never ran: MTemp moved, the cursor didn't.

TEST_NAME := IIcx Display Card 8•24 GC — ADB mouse deltas move the cursor
TEST_DESC := ADB delta injection (web2 route) updates Mouse/RawMouse via the slot VBL cursor task

TEST_ROM := roms/iix-iicx-se30-97221136.rom

TEST_ARGS := model=iicx ram=8192
