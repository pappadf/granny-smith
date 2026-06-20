# Integration test configuration: Apple Lisa 2 — LOS 3.1 mouse-driven Install click
# Boots the Lisa 2 (rev-H boot ROM) to the LOS 3.1 Install/Repair/Restore menu,
# then drives the COPS mouse to warp the cursor onto the "Install" button and
# clicks it — exercising the full host→COPS→OS mouse pipeline against the real OS.

TEST_NAME := Apple Lisa 2 LOS 3.1 mouse-driven Install
TEST_DESC := Warps the cursor to the Install button and clicks it; verifies the installer responds (reaches its disk-selection screen)

# Interleaved rev-H Lisa 2 boot ROM (341-0175-H / 341-0176-H), 16 KB, checksum 0x098917B2.
TEST_ROM := Lisa/roms/098917B2-LisaH.rom

# Lisa 2, 2 MB.  Same boot as lisa-los-boot; this test adds the mouse interaction.
# The cursor is positioned by the "global" warp: the COPS reads the OS's live
# cursor globals ($CC00F0 = X, $CC00F2 = Y) each mouse report and emits scale-
# corrected deltas (the OS scales X by 3/2, Y by 1) until the cursor reaches the
# target pixel; a button click is then the COPS mouse-button keycode, which the OS
# hit-tests against the tracked cursor.
TEST_ARGS := model=lisa ram=2048 fd=$(TEST_DATA)/Lisa/LisaOfficeSystem-3.1/LOS-3.1-1.image
