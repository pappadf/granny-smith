# AV video-in unit test.
# Links the real av/vdc.c + av/civic.c + av/cuda.c against recording stubs
# and pins: the Cuda $22 RdWrIIC wire framing (direction bit, subaddress,
# multi-byte auto-increment, unknown-slave error packet), the golden DMSD/
# VDC open-sequence replay, the two synthesized status bytes, and the frame
# engine (window/decimation/format packing, VidInSize stride, the $0001FEFF
# liveness overwrite, interrupt polarity + 0-then-1 ack, VDCClk freezing).

TEST_NAME    := vdc
TEST_SRCS    := test.c
TEST_HARNESS := none
STUBS        := memory checkpoint display_class
EXTRA_SRCS   := ../../../../src/machines/av/vdc.c \
                ../../../../src/machines/av/civic.c \
                ../../../../src/machines/av/cuda.c
include ../../common.mk
