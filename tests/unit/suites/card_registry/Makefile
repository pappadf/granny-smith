# card_registry.c: the registry helpers NuBus and PCI share -- find by id
# (NULL and "" refused), the underscore-insensitive did-you-mean, the
# refusal formatter, and the one check of a slot entry's monitor against a
# kind's monitor rows (and the sense the row drives).  See test.c.

TEST_NAME    := card_registry
TEST_SRCS    := test.c
TEST_HARNESS := none
STUBS        := assert
EXTRA_SRCS   := ../../../../src/core/peripherals/card_registry.c \
                ../../../../src/core/object/value.c
include ../../common.mk
