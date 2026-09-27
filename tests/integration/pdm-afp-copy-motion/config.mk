# Integration test: an AFP copy on Mac OS 8.1 (pm6100) while the mouse moves
#
# The reproduction of issue #124: pm6100 + Mac OS 8.1, a host share mounted
# through the Chooser, a ~400 KB file dragged to the desktop -- and the
# mouse kept moving (real ADB deltas, i.e. Cuda autopoll packets) for the
# whole copy.  Before 2ac5e945 the AMIC's SCC receive DMA charged each
# frame's wire time a second time; 8.1's LocalTalk driver polls that engine
# at IPL 1, so an autopoll packet landing inside the 20 ms blackout
# desynchronised the Cuda byte stream and Ticks froze for good.  The row
# asserts the opposite: Ticks advance through every motion round and every
# byte of the file is read.
#
# MEDIA-GATED: the 8.1 guest image is gs-test-data extended media
# (apps/quake_8_1_voodoo2.img.7z, the same image tnt-voodoo2-glide boots);
# the row skips cleanly when it is not fetched.

TEST_NAME := PDM AFP copy with mouse motion
TEST_DESC := pm6100 + Mac OS 8.1: drag a file off an AppleShare volume while the mouse keeps moving; Ticks never stall and the copy completes (#124).

TEST_ROM := roms/pm6100-pm7100-pm8100-9feb69b3.rom

# A private copy of the system (the guest writes to it), made only when
# the fetched image exists (the skip path needs no media); a fresh share
# (the server keeps its catalog in "<share>/.gs-afp") holding a payload
# big enough for a run of multi-frame ATP bursts -- the 410 KB MacTest
# floppy image is always-fetched test data, its content is irrelevant.
TEST_SETUP := (test ! -f "$(TEST_DATA)/apps/quake_8_1_voodoo2.img" || cp "$(TEST_DATA)/apps/quake_8_1_voodoo2.img" "$(WORK_DIR)/macos81.img") && rm -rf "$(WORK_DIR)/share" && mkdir -p "$(WORK_DIR)/share" && cp "$(TEST_DATA)/apps/MacTest-Plus.image" "$(WORK_DIR)/share/payload.image"

TEST_ARGS := model=pm6100 ram=24576

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := extended
