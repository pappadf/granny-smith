# Makefile for host-native peeler tool
# Delegates to the peeler submodule build and copies the binary here.

PEELER_DIR := ../../third-party/peeler
TOOL       := peeler

.PHONY: all clean

all: $(TOOL)

$(TOOL):
	$(MAKE) -C $(PEELER_DIR)
	cp $(PEELER_DIR)/build/peeler $@

clean:
	$(MAKE) -C $(PEELER_DIR) clean
	rm -f $(TOOL)
