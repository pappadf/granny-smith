# Integration test: the beige G3 family suite — Mac OS 9.2.1 from a MESH
# disk to the desktop on both models.

TEST_NAME := Gossamer suite (G3 desktop / mini tower)
TEST_DESC := Mac OS 9.2.1 MESH-disk boots to the desktop with the startup sound on the beige G3 desktop and mini tower

TEST_ROM := roms/pmg3dt-pmg3mt-78f57389.rom

TEST_ARGS := model=pmg3dt ram=65536

# Two boots of ~12 G instructions each.
TEST_TIMEOUT := 3600

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := matrix
