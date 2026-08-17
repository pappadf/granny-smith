# Integration test configuration: IIfx + Display Card 8•24 — format a blank
# HD160SC under Mac OS 7.6, run the 7.6 Installer to completion, and boot the
# installed volume.
#
# Boots a Macintosh IIfx (16 MB) with the Apple Macintosh Display Card 8•24
# (JMFB, mdc_8_24 — loads mdc-8-24-revb-d1629664.vrom next to the ROM) at 13"
# RGB 640x480 8 bpp from the Mac OS 7.6 "Disk Tools 1" floppy, runs Apple HD SC
# Setup v7.3.5 against a freshly-created blank HD160SC (QUANTUM ELS170S,
# 177,269,760 bytes) at SCSI ID 0, initialises it, reboots from the Mac OS 7.6
# "Install 1" floppy, switches the Installer to Custom Install, selects
# "Minimum system for any supported computer", and feeds all fourteen source
# floppies it asks for — 2, 3, 4, 5, 6, 7, 8, 9, 10, 14, 17, 18, 19 and finally
# "Install 1" back — ending on the Installer's own "Installation was
# successful." dialog and a boot from the installed volume to the Finder
# desktop.
#
# Each request is matched pixel-exact against a committed full-screen reference
# BEFORE the disk goes in, so the screenshot is both the synchronisation point
# and the assertion that the Installer asked for that particular disk.  Run
# with `--var REGEN=1` to recapture the references.
#
# Regression guard for the SWIM IOP 24-bit DMA-address fix
# (src/machines/oss/iop_swim.c, swim_host_dma_ptr).  Mac OS 7.6 runs the IIfx
# in 24-bit addressing mode (68030 PMMU tree, TC.IS=8), and the Installer hands
# the .Sony driver a *locked* handle's master pointer as the read buffer — high
# byte $80, i.e. $80592D18 for a block at $00592D18.  Those flag bits never
# reach the DMA controller on real hardware (24 address lines), but the IOP
# model took the buffer address raw, so its RAM bounds check rejected the
# transfer with paramErr (-50); the Installer gave up with "An error occurred
# while trying to complete the installation."  With the strip in place the read
# lands and the Installer carries on across every swap.
#
# Also covers system_fd_insert's explicit-drive contract: every swap names its
# drive, and an occupied drive is now an error rather than a silent redirect to
# the other one.
#
# RUNTIME: about 5 minutes at --speed=max — fourteen floppy reads plus two
# boots, ~3.7 billion guest instructions.  Longer than most of the suite.
#
# WHY MINIMUM SYSTEM AND NOT EASY INSTALL: an Easy Install cannot complete from
# this floppy set.  "Installation Tome 7" holds "English Dialect" (an
# AppleScript dialect resource, type 'dlct' creator 'ascr') whose compressed
# stream is corrupt in the media — about 25.9% of its 42,024 bytes are damaged.
# That is not an emulator defect: a reference decompressor written on the host
# reproduces 277 of 278 single-chunk forks across all 14 tomes exactly, and the
# floppy copy of this one decompresses byte-for-byte identically to a known-good
# plaintext (from the 7.6 net-install image, where the file is stored
# uncompressed) for 27,787 bytes before diverging.  The file is reachable from
# three Installer packages — "Universal system…", "System for this computer"
# (Easy Install's choice) and "Utility" → "AppleScript" — so deselecting
# AppleScript alone does not avoid it; "Minimum system for any supported
# computer" is the one system variant that excludes it.  Full analysis:
# the IIfx 7.6-installer decompressor investigation notes
#
# Companion: tests/integration/iici-format-hd (System 6.0.8 / HD20SC / HD SC
# Setup 2.0.3) pins the IIcx's NCR 5380 pseudo-DMA primer gate.
#
# Media note: tests/data/systems/SSW-7.6-1.4M/ holds all 20 floppies as plain
# 1,474,560-byte HFS images (already decoded from the original NDIFs — no
# AppleDouble "._<name>" sidecars are needed or present).  This test uses 15 of
# them: Disk Tools 1 and Install 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 14, 17, 18, 19.
# All are persisted in the private gs-test-data repo and arrive via
# scripts/fetch-test-data.sh.
#
# ⚠️ The floppies are mounted writable, so the guest updates their HFS Master
# Directory Block (sector 2 — mount/modify stamps).  Under the integration
# harness those writes land in GS_STORAGE_CACHE and tests/data is left
# untouched, which is what keeps the run reproducible.  Driving the emulator by
# hand from the repo root does NOT set that cache and will write through to the
# shared images (and scatter .delta/.journal files beside them); restore with
# scripts/fetch-test-data.sh before capturing references.

TEST_NAME := IIfx Mac OS 7.6 — format HD160SC, complete Custom Install, boot it
TEST_DESC := Format a blank HD160SC with Apple HD SC Setup off the 7.6 Disk Tools floppy, run a Custom Install of the minimum system across all 14 source floppies with a reference screenshot per swap, then boot the installed volume to the Finder desktop

TEST_ROM := roms/iifx-4147dd77.rom

# The harness only creates the machine; the script re-boots it with the 8•24
# card and the 640x480 8 bpp mode selected (video_card / video_mode can't be
# passed as boot args here), creates and attaches the blank HD, and inserts
# each floppy itself.
TEST_ARGS := model=iifx ram=16384

# CI tier (proposal-integration-test-rework §5.4): unit | matrix | extended
TEST_TIER := extended
