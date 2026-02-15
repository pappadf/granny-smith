# Integration test: Boot to desktop
# Tests that the emulator can boot System 6.0.8 to the Finder desktop
#
# This Makefile allows running the test standalone. The parent Makefile
# (tests/integration/Makefile) uses config.mk for auto-discovery.

ROOT := ../../..
HEADLESS := $(ROOT)/build/headless/gs-headless
TEST_DATA := $(ROOT)/tests/data
STORAGE_CACHE := $(ROOT)/build/storage-cache

# Include test configuration
include config.mk

.PHONY: test capture clean

# Run the integration test
test: $(HEADLESS)
	@echo "Running: $(TEST_NAME)"
	@GS_STORAGE_CACHE=$(STORAGE_CACHE) $(HEADLESS) \
		rom=$(TEST_DATA)/$(TEST_ROM) \
		$(TEST_ARGS) \
		script=test.script \
		--speed=max
	@echo "PASS: $(TEST_NAME)"

# Capture a new reference screenshot
capture: $(HEADLESS)
	@echo "Capturing reference desktop image..."
	@GS_STORAGE_CACHE=$(STORAGE_CACHE) $(HEADLESS) \
		rom=$(TEST_DATA)/$(TEST_ROM) \
		$(TEST_ARGS) \
		script=capture.script \
		--speed=max
	@echo "Captured: desktop.png"

# Build headless emulator if needed
$(HEADLESS):
	$(MAKE) -C $(ROOT) -f Makefile.headless

clean:
	rm -f desktop.png
