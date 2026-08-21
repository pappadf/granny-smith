# Integration test configuration: Power Macintosh 8100 + Display Card 24AC
# (genuine video ROM) — System 7.5 + the Symantec "System Info" Display
# benchmark.
#
# The PowerPC sibling of iicx-24ac-sysinfo, and the first test anywhere to
# run the GENUINE 24AC declaration ROM behind BART: suite-pdm's
# 8100-75-24ac row seats the generic-vROM twin ("24ac"), whose card never
# gets mode-set past 1 bpp, so the real card's PrimaryInit and its own 68K
# video driver had no PowerPC coverage at all.  Here the real vROM
# (roms/display-card-24ac-d8daab87.vrom, bound by the shared declrom
# loader's content-addressed catalog) runs in a NuBus slot of an 8100 and
# brings its framebuffer up at 640x480x8 under System 7.5.
#
# READ THIS BEFORE ADDING PIXEL ASSERTIONS ABOUT THE CARD.  The PDM family
# always has a monitor strapped to its HDI-45, so the built-in Ariel video
# is the main screen and the card comes up as screen two; `machine.screen`
# follows the machine's built-in display, so EVERY golden in this test is
# the ON-BOARD framebuffer, and System Info benchmarks the main device —
# its "Test Display" row reads "Built-in video (8 bit)" and is not a
# picker.  The card's contribution is asserted through the object model and
# the guest's own GDevice list instead, which is where it is observable.
#
# Both fixtures are provisioned into tests/data by scripts/fetch-test-data.sh:
# the 77 MB System 7.5 HD as systems/system_7_5_0_77mb_mode32_24ac.img.7z
# (auto-extracted, shared with suite-pdm and five other suites) and the
# 1.44 MB floppy as apps/Norton-Utils-Disk-2.image (raw).  No TEST_SETUP is
# needed — the media layer mints a delta instance for the writable mount.

TEST_NAME := Power Macintosh 8100 + Display Card 24AC — System 7.5 System Info Display benchmark
TEST_DESC := Boot pm8100 with a genuine-vROM 24AC in NuBus slot $$C to the 7.5 Finder, insert "Utilities Disk 2", launch the PowerPC-only System Info off the floppy, select only Test Display, and run the Display benchmark to its System Ratings result

# 4 MB Power Macintosh 6100/7100/8100 ROM (stored checksum 0x9FEB69B3).
TEST_ROM := roms/pm6100-pm7100-pm8100-9feb69b3.rom

# 16 MB is the 8100's second ram_options rung (8 MB soldered + paired
# 16 MB SIMMs); it matches suite-pdm's 8100 rows so their boot timings
# transfer directly.  video_card can't be passed as an arg and the slot
# staging channel only exists once a machine with that socket is built, so
# the script boots twice — see its header.
TEST_ARGS := model=pm8100 ram=16384

# CI tier (proposal-integration-test-rework §5.4): unit | matrix | extended
# Extended, like the two IIcx sysinfo tests: this asserts an application's
# behaviour rather than a (machine x system x card x geometry x depth)
# cell, so it is deliberately NOT declared in matrix-targets.json.
TEST_TIER := extended
