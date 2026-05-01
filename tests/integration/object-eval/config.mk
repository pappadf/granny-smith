# Integration test: object-model `eval` command (M2)
# Verifies that the new gs_eval entry point resolves paths against a
# booted Plus image and prints the expected JSON shapes. The legacy
# shell continues to operate alongside.

TEST_NAME := Object-model eval (Plus)
TEST_DESC := Smoke test for `eval` shell command — cpu/memory/machine paths

TEST_ROM := roms/Plus_v3.rom
