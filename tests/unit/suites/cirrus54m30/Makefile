# The Apple Network Server's on-board Cirrus Logic 54M30 PCI framebuffer
# (src/core/peripherals/pci/cards/cirrus54m30.c).  Links the real card and
# the real PCI config-space header (config_space.c), so the BAR-sizing,
# expansion-ROM and interrupt-pin rows read what a guest's config cycles
# read; test.c stubs the rest of the PCI bus, the object tree, the scheduler
# and the checkpoint stream.  Pins the mode the card derives from its
# CRTC/sequencer/extension registers, the register file's VGA port
# behaviour, and a checkpoint round-trip of the mode.  Under ASan + UBSan,
# since every row drives guest-programmed geometry into a scanout over a
# fixed 1 MB store.

TEST_NAME     := cirrus54m30
TEST_SRCS     := test.c
TEST_HARNESS  := none
EXTRA_SRCS    := ../../../../src/core/peripherals/pci/cards/cirrus54m30.c \
                 ../../../../src/core/peripherals/pci/config_space.c
SANITIZE      := $(if $(filter emcc%,$(notdir $(firstword $(CC)))),,-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
RUN_ENV       := ASAN_OPTIONS=detect_leaks=1
include ../../common.mk
