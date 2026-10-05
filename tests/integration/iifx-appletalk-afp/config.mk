# Integration test configuration: AppleTalk through the IIfx's SCC IOP
#
# appletalk-afp-sys7's AppleShare session, run on a IIfx.  There AppleTalk
# does not drive the SCC: the System's IOP LocalTalk 'ltlk' downloads the
# IOP LocalTalk driver into the SCC IOP and talks LLAP through its
# mailboxes, which the SCC IOP model bridges to the emulated network
# (src/machines/oss/iop_scc.c).  Before that model existed every node the
# driver probed read as taken, and the System reported "AppleTalk cannot be
# used because no AppleTalk address is available".

TEST_NAME := AppleTalk AFP through the IIfx SCC IOP
TEST_DESC := Acquire a LocalTalk node through the IIfx's SCC IOP, mount a host share from System 7.5 through the Chooser, copy a file from it and resolve an alias on it.

TEST_ROM := roms/iifx-4147dd77.rom

# A private copy of the system (the guest writes to it), and a fresh share:
# the server keeps its catalog in "<share>/.gs-afp".
TEST_SETUP := cp "$(TEST_DATA)/systems/system_7_5_0_77mb_mode32_24ac.img" "$(TEST_TMPDIR)/hd.img" && rm -rf "$(WORK_DIR)/share" && mkdir -p "$(WORK_DIR)/share" && printf 'forty\n' > "$(WORK_DIR)/share/A name that runs to forty characters, ok" && printf 'thirty-one\n' > "$(WORK_DIR)/share/Thirty-one characters, exactly."

TEST_ARGS := model=iifx ram=16384 hd=$(TEST_TMPDIR)/hd.img

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := extended
