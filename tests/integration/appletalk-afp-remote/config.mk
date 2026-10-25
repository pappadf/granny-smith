# Integration test configuration: a second AFP server with a remote volume
#
# A file server beside "Shared Folders" (appletalk.remote.add_server), with
# a read-only volume whose content is a host folder served through the
# dir:// provider -- the same path the page's archive.org and web-index
# providers take, with the core answering instead of the page.  The guest
# finds the second server in the Chooser, mounts its volume, and opens a
# folder that is a StuffIt archive in a BinHex wrapper holding a disk image:
# the server fetches it, unpacks it and shows the disk's files.

TEST_NAME := AppleTalk AFP remote server
TEST_DESC := Chooser -> AppleShare -> a second server -> mount a remote volume -> open an archive as a folder.

TEST_ROM := roms/plus-v3-4d1f8172.rom

# The source is what the remote holds; the cache is the volume's host folder,
# fresh per run so nothing is served from an earlier one.
TEST_SETUP := cp "$(TEST_DATA)/systems/system_6_0_8_20mb_8_24gc.img" "$(TEST_TMPDIR)/hd.img" && rm -rf "$(WORK_DIR)/source" "$(WORK_DIR)/cache" && mkdir -p "$(WORK_DIR)/source/Docs" && cp "$(TEST_DATA)/apps/MacTest_Disk.image_.sit_.hqx" "$(WORK_DIR)/source/" && printf 'Served from afar.\n' > "$(WORK_DIR)/source/Read Me" && printf 'Notes.\n' > "$(WORK_DIR)/source/Docs/Notes"

# AppleTalk starts inactive, as in appletalk-afp: the Chooser activates it.
TEST_ARGS := config=appletalk-inactive.json hd=$(TEST_TMPDIR)/hd.img

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := extended
