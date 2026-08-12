# Integration test: top-level objects (appletalk, mouse)
# Exercises the AppleTalk/AFP object surface — stack enablement, the AFP
# server node, the volumes collection with its constructive add and name
# lookup, the NBP registry view, the observability subtrees and the
# printer's attribute pair — plus mouse.{move,click,trace}.

TEST_NAME := Object-model top-level (appletalk + mouse)
TEST_DESC := appletalk.afp volumes/sessions/stats + .printer attributes + mouse.* methods

TEST_ROM := roms/plus-v3-4d1f8172.rom

# CI tier (proposal-integration-test-rework §5.4): unit | matrix | extended
TEST_TIER := unit
