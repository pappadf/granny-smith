# Integration test: fork-preserving archive extraction
#
# Unpacking a Mac archive (here a StuffIt fixture that ships in-tree with the
# peeler sources) must preserve each file's resource fork + Finder Info as an
# AppleDouble "._<name>" sidecar, not discard it. See
# proposal-appledouble-support.md §Phase 3 and src/core/storage/archive.c.

TEST_NAME := Archive fork-preserving unpack
TEST_DESC := archive.extract a StuffIt fixture; verify AppleDouble "._<name>" sidecars carry the resource fork.

TEST_ROM := roms/iix-iicx-se30-97221136.rom
