# Integration test configuration: an AppleShare mount from System 7.5
#
# Every other AFP row mounts from System 6, which speaks AFP 2.0.  This one
# mounts the same kind of share from System 7.5 on a IIci (the aevt rows'
# machine and image), so a real client negotiates with the server from the
# System 7 side (10-network D-8), and it copies a file whose host name is
# longer than a Mac name to the Mac's own disk -- D-5's measurement, repeated
# on System 7.

TEST_NAME := AppleTalk AFP from System 7.5
TEST_DESC := Mount a host share from System 7.5 through the Chooser, check the negotiated AFP version, and copy a 40-character-named file to the Mac's disk (10-network D-8).

TEST_ROM := roms/iici-368cadfe.rom

# A private copy of the system (the guest writes to it), and a fresh share:
# the server keeps its catalog in "<share>/.gs-afp".
TEST_SETUP := cp "$(TEST_DATA)/systems/system_7_5_0_77mb_mode32_24ac.img" "$(TEST_TMPDIR)/hd.img" && rm -rf "$(WORK_DIR)/share" && mkdir -p "$(WORK_DIR)/share" && printf 'forty\n' > "$(WORK_DIR)/share/A name that runs to forty characters, ok" && printf 'thirty-one\n' > "$(WORK_DIR)/share/Thirty-one characters, exactly."

TEST_ARGS := model=iici ram=8192 hd=$(TEST_TMPDIR)/hd.img

# CI tier (proposal-integration-test-rework §5.4): unit | matrix | extended
TEST_TIER := extended
