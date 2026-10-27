# Integration test: Quake III Arena's demo four on a Rage 128, timed, with
# the PowerPC translation tier
#
# g3-rage128-quake3-demo (its test.script, unchanged) under the headless
# program built as WebAssembly and run by Node (Makefile.headless
# TARGET=node), with the predecoded loop and the JIT on, supervisor code
# included: the CPU side of the browser's "JIT + kernel".  The 3D is still
# drawn by the software rasteriser (no WebGPU under Node).  The two rows'
# timings side by side are the interpreter and the JIT on one host.
#
# run.sh builds the Node program when emcc and node are there (an
# incremental make: a no-op when it is current), uses a built one
# otherwise, and skips without either.

TEST_NAME := G3 Quake III demo on a Rage 128 (JIT)
TEST_DESC := Quake III Arena 1.30 plays its demo four on the Rage 128 GL with the PowerPC JIT (the wasm build under Node), timed in host and guest time with the frame rate

TEST_ROM := roms/pmg3dt-pmg3mt-78f57389.rom

TEST_RUNNER := run.sh

# The build (when it is needed), the boot, Quake's start and the demo.
TEST_TIMEOUT := 3600

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := extended
