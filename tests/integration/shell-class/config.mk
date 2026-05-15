# Integration test: Shell class on the object root
# (proposal-shell-as-object-model-citizen.md). Verifies that the
# `shell.*` surface — attributes (prompt / running / aliases / vars) and
# methods (run / complete / expand / script_run / alias_set /
# alias_unset / interrupt) — is reachable through gs_eval and behaves
# the way the proposal describes.

TEST_NAME := Shell class surface
TEST_DESC := shell.{run,complete,expand,alias_set,alias_unset,vars,aliases,prompt,running} via gs_eval

TEST_ROM := roms/Plus_v3.rom
