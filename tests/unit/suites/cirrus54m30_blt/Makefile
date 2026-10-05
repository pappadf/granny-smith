# The Cirrus 54M30's BitBLT engine and its legacy-window mirror
# (src/core/peripherals/pci/cards/cirrus54m30.c), which NT's cirrus.dll draws
# every pixel of GUI Setup through.
#
# Links the real card against the real pci.c/config_space.c and a stub bus
# environment of its own, so the engine is driven the way the machine drives
# it: through a bridge window with lane reversal on, at the offsets Appendix
# B20 documents.  The card's mode, config space, DAC and checkpoint rows are
# suites/cirrus54m30.
#
# -Wno-unused-parameter: the unit harness replaces LOG() with a macro that
# discards its arguments, so parameters a function uses only in a log line
# look unused in THIS build alone.

TEST_NAME     := cirrus54m30_blt
TEST_SRCS     := test.c
TEST_HARNESS  := none
EXTRA_SRCS    := ../../../../src/core/peripherals/pci/pci.c \
                 ../../../../src/core/peripherals/pci/config_space.c \
                 ../../../../src/core/peripherals/pci/cards/cirrus54m30.c
STUBS         := checkpoint prom
EXTRA_CFLAGS  := -Wno-unused-parameter
include ../../common.mk
