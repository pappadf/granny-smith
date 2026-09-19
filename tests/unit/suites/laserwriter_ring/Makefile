# laserwriter_transport_ring.c — the browser build's transport between the
# LaserWriter bridge and its interpreter worker (laserwriter_ring_protocol.h).
#
# The suite plays the worker in C: it reads OPEN / FEED / FINISH / ABANDON
# records from the outbound ring and writes OPENED / FED / FINISHED into the
# inbound ring, and checks the bridge-side callbacks.  The rings are made
# small (the protocol header lets a build override the sizes) so the tests
# can force both rings to wrap, a record that needs a PAD before the ring's
# end, and a ring with no room.
TEST_NAME := laserwriter_ring
TEST_SRCS := test.c
TEST_HARNESS := isolated
EXTRA_SRCS := ../../../../src/core/network/laserwriter_transport_ring.c
EXTRA_CFLAGS := -DLWRING_OUT_BYTES=8192u -DLWRING_IN_BYTES=4096u
include ../../common.mk
