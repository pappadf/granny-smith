# Integration test configuration: the debugger on a running Lisa.
#
# Boots LOS 3.1 from the installed ProFile to the Office System desktop --
# four MMU contexts in use -- and checks what the debugger answers there:
# translate / walk / map / peek in an explicit context, the memory views in
# a context and physically, a breakpoint in a segment that is not present,
# physical memory logpoints, Mac trap naming kept off, and the soft
# power-off.  TEST_SETUP copies the ProFile so the original stays pristine.

TEST_NAME := Lisa debugger
TEST_DESC := MMU contexts, memory views, breakpoints, logpoints and power-off on a running LOS 3.1

TEST_ROM := roms/lisa2-revh-098917b2.rom
TEST_ARGS := model=lisa ram=2048

TEST_SETUP := cp "$(TEST_DATA)/Lisa/LisaOfficeSystem-3.1/LOS-3.1-ProFile.image" "$(WORK_DIR)/profile.image"

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := matrix
