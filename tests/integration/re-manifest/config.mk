# Integration test: full re.dump pipeline including manifest.json,
# decoded/*, symbols.txt, and the --no-disasm / --no-decode flags.

TEST_NAME := re.dump manifest + decoders + flags
TEST_DESC := Validate manifest schema, decoded JSON, and the --no-X flags on System 6 Finder
TEST_ROM := roms/Plus_v3.rom
TEST_RUNNER := run.sh
