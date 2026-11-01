# Integration test configuration: AFP across two routed instances
#
# Two headless instances, A and B, each its own LocalTalk network with its
# own router, linked over UDP on the loopback interface.  Both networks are
# in one zone, "Granny Smith" (the shared zone mode), and B shares its host
# services with A (share_host_services, a friend link).  A's guest -- System
# 6 on a Plus, with its own host's file server off -- opens the Chooser:
# NBP's lookup goes out as a BrRq, A's router forwards it to B's network as
# a FwdReq, B's router turns it into a local lookup that B's host answers,
# and the reply comes back through both routers.  A's guest then logs in as
# Guest and mounts B's volume: ATP, ASP and AFP over the routed link.

TEST_NAME := AppleTalk AFP across routers
TEST_DESC := A guest on one instance finds and mounts the file server of another, through both routers over UDP.

TEST_ROM := roms/plus-v3-4d1f8172.rom

TEST_SETUP := cp "$(TEST_DATA)/systems/system_6_0_8_20mb_8_24gc.img" "$(TEST_TMPDIR)/hd.img" && mkdir -p "$(WORK_DIR)/share" && printf 'Shared over two routers.\n' > "$(WORK_DIR)/share/README"

TEST_RUNNER := run.sh

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := extended
