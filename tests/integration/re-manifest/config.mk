# Integration test: full tools/dump pipeline including manifest.json,
# decoded/*, symbols.txt, and the --no-disasm / --no-decode flags.

TEST_NAME := dump manifest + decoders + flags
TEST_DESC := Validate manifest schema, decoded JSON, and the --no-X flags on System 6 Finder
TEST_ROM := roms/plus-v3-4d1f8172.rom
TEST_RUNNER := run.sh
