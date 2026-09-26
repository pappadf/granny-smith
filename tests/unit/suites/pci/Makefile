# Generic PCI core unit test.
# Links the real core/peripherals/pci/{pci,config_space}.c against a stub
# bus environment and pins the config-cycle contract: absent devices read
# all-ones, the type-0 header assembles from the declaration, BAR sizing
# works by masked read-back, the command register gates the decode, the
# expansion-ROM enable bit is honoured, bridge windows dispatch to the
# right device and fault on everything else, and staged slot picks resolve
# by the documented precedence.

TEST_NAME    := pci
TEST_SRCS    := test.c
TEST_HARNESS := none
STUBS        := checkpoint
EXTRA_SRCS   := ../../../../src/core/peripherals/pci/pci.c \
                ../../../../src/core/peripherals/pci/config_space.c
include ../../common.mk
