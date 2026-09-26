# The Voodoo2's scanout raster against the largest videoDimensions a guest
# can program (src/core/peripherals/pci/cards/voodoo2.c).  Links the real
# card, its rasterisers and the value layer; test.c stubs the PCI bus, the
# object tree and the scheduler.  Natively under ASan + UBSan: the defect
# this pins was a host heap write past the scanout allocation, which
# nothing short of a sanitizer sees.

TEST_NAME     := voodoo2_scanout
TEST_SRCS     := test.c
TEST_HARNESS  := none
STUBS         := checkpoint assert
EXTRA_SRCS    := ../../../../src/core/peripherals/pci/cards/voodoo2.c \
                 ../../../../src/core/peripherals/pci/cards/voodoo2_raster.c \
                 ../../../../src/core/peripherals/pci/cards/voodoo2_gpu.c \
                 ../../../../src/core/object/value.c
SANITIZE      := $(if $(filter emcc%,$(notdir $(firstword $(CC)))),,-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
EXTRA_LDFLAGS := -lpthread
RUN_ENV       := ASAN_OPTIONS=detect_leaks=1
include ../../common.mk
