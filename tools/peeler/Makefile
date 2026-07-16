# Makefile for host-native peeler tool
# Delegates to the in-tree peeler build (src/peeler) and copies the binary here.

PEELER_DIR := ../../src/peeler
TOOL       := peeler

.PHONY: all clean

all: $(TOOL)

$(TOOL):
	$(MAKE) -C $(PEELER_DIR)
	cp $(PEELER_DIR)/build/peeler $@

clean:
	$(MAKE) -C $(PEELER_DIR) clean
	rm -f $(TOOL)
