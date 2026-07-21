# Integration test configuration: IIcx + Display Card 8•24 GC ("Dolphin").
#
# Verifies the stage-0/1 HLE card model: the card is detected as an 8•24 GC
# (genuine v1.1 declaration ROM 341-0266, BoardId $2C, loaded via the new
# lane-0 byteLanes-$E1 support), the object-model surface, and the accelerator
# command-block (CB) bring-up protocol — which the test drives directly by
# writing the bytes the .GraphAccel driver would (boot handshake + an RPC).
#
# It does NOT boot the OS — that is covered end-to-end by iicx-824gc-accel,
# which boots System 6.0.8 through the genuine decl-ROM video driver and the
# real .GraphAccel bring-up to the Finder desktop.  This test drives the
# accelerator CB protocol directly, so no SCSI HD image is needed.

TEST_NAME := IIcx Display Card 8•24 GC — card model + accelerator CB bring-up
TEST_DESC := Detect the 8•24 GC card (v1.1 decl ROM, lane-0) + drive the HLE CB bring-up protocol

TEST_ROM := roms/iix-iicx-se30-97221136.rom

# The harness creates the IIcx with 8 MB; the script re-boots with the 8•24 GC
# card selected (video_card can't be passed as an arg).
TEST_ARGS := model=iicx ram=8192
