# Integration test: checkpoints of the running machine, and find_media, on
# headless.
#
# Registering a machine, the quick checkpoint, clearing it and finding media
# in a directory are file work in core, the same on every platform.  They
# used to exist only in the wasm platform, behind weak stubs that told
# headless "only supported in the WASM build", so nothing could test them
# outside a browser.  What really is browser-only -- handing a file to the
# browser as a download -- now says "not supported on this platform".

TEST_NAME := Checkpoint platform seam
TEST_DESC := machine.register, checkpoint.snapshot/probe/clear and storage.find_media work headless; download says it is not supported

TEST_ROM := roms/plus-v3-4d1f8172.rom

TEST_SETUP := mkdir -p "$(WORK_DIR)/cp" "$(WORK_DIR)/media" && cp "$(TEST_DATA)/systems/System_3_2_0.dsk" "$(WORK_DIR)/media/"
TEST_ARGS := --checkpoint-dir=$(WORK_DIR)/cp

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
