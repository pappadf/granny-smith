# Integration test: the ATI Rage 128 GL's 3D engine
#
# PCI Phase 4, milestone 4f.  Triangles, lines and points go through the
# CCE as 3D_RNDR_GEN_PRIM / 3D_RNDR_GEN_INDX_PRIM packets with the 3D
# state programmed register by register, the way Mesa's r128 driver
# programs it; every assertion is an EQUALITY on VRAM (colour buffer and
# Z/stencil buffer) — coverage under the chosen fill rule, Gouraud values,
# texel selection, combine, blend, alpha test, Z, stencil, fog, culling.
# No guest code runs.

TEST_NAME := Rage 128 3D engine
TEST_DESC := Rage 128 GL triangles, lines and points through the CCE: coverage, shading, texturing, combine, blend, alpha test, Z, stencil, fog and culling, checked as VRAM equalities

# 4 MB Power Macintosh 7500/8500/9500 ROM v1 (stored checksum 0x96CD923D)
TEST_ROM := roms/pm7500-pm8500-pm9500-96cd923d.rom

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
