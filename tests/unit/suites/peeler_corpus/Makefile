# peeler's real-archive corpus, run as part of the unit tier.
#
# src/peeler/test/testfiles holds 22 genuine archives -- Compact Pro 1.33 and
# 1.52; StuffIt 4.5, 6.5.1 and 7 as .sit and .sea, each raw, MacBinary and
# BinHex; DropStuff 6 at both settings -- with the md5sums of what they
# extract to, and src/peeler/test/run_tests.sh checks them through the peeler
# CLI.  Until this suite, nothing ran it.  Batch 9's first fixes passed every
# synthetic test in suites/peeler and broke three of these archives (09
# WORK-ORDER §10): synthetic fixtures show a fix refuses bad input; only real
# archives show it still accepts good input.
#
# The CLI is built with peeler's own Makefile, so its flags --
# -std=c99 -Wpedantic -Werror -- are enforced here too.

UNIT_ROOT      := $(abspath ../../)
WORKSPACE_ROOT := $(abspath $(UNIT_ROOT)/../..)
PEELER_DIR     := $(WORKSPACE_ROOT)/src/peeler

BUILD_DIR ?= $(UNIT_ROOT)/build
OUT       := $(BUILD_DIR)/peeler_corpus

.PHONY: all run clean

all:
	@$(MAKE) --no-print-directory -C $(PEELER_DIR) BUILD=$(OUT)/build

run: all
	@rm -rf $(OUT)/out
	@bash $(PEELER_DIR)/test/run_tests.sh --peeler $(OUT)/build/peeler --output-dir $(OUT)/out

clean:
	rm -rf $(OUT)
