# Integration test: Mac OS X 10.0.3 boots a beige G3 to the Finder from a
# UDIF disk on the ATA bus.
#
# The disk is gs-test-data `systems/mac_os_x_10_0_3_2gb_g3_minimal.dmg`, a
# minimal install with automatic login.  Its sidecar .json is the machine
# configuration it was made with; the row boots it WITHOUT the sidecar's
# nvram block, as the New Machine dialog does: the startup device names the
# disk's Mac OS X volume (partition 3 behind the GSDisk map), and the new
# G3's seeding writes Apple's Gossamer patch and that boot-device itself.
# The sidecar's 17" multiple-scan monitor stays.

TEST_NAME := G3 Mac OS X 10.0.3 to the Finder
TEST_DESC := Boots the minimal Mac OS X 10.0.3 disk (UDIF, ATA) on a beige G3 through BootX and the kernel to the logged-in Finder at 1024 x 768

TEST_ROM := roms/pmg3dt-pmg3mt-78f57389.rom

# The configuration comes with the disk.  Test data that predates the disk
# has neither, so the setup stands in an empty document, the machine boots
# with its defaults and the script skips the row.
TEST_SETUP := python3 -c 'import json, sys; c = json.load(open(sys.argv[1])); c.pop("nvram", None); c["startup"] = {"bus": "ata0", "unit": 0, "partition": 3}; json.dump(c, open(sys.argv[2], "w"))' "$(TEST_DATA)/systems/mac_os_x_10_0_3_2gb_g3_minimal.json" "$(TEST_TMPDIR)/config.json" 2>/dev/null || echo '{}' > "$(TEST_TMPDIR)/config.json"

TEST_ARGS := model=pmg3dt ram=262144 config=$(TEST_TMPDIR)/config.json

# One boot: the Finder is up after 20 G of scheduler time.
TEST_TIMEOUT := 1800

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := matrix
