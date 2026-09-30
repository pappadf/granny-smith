# Integration test: structured-result annotations in the job record stream.
#
# The text a job prints does not change; `value_begin` / `value` records
# bracket a value the REPL printed and an `error` record marks a statement
# error, at their positions among the `output` records.  Driven through
# headless --framed, which prints them as @value_begin / @value / @error
# lines among the @out lines.  Also pins the record bound: every record fits
# a quarter of the event ring, so 64 KiB of control bytes (6x on escaping)
# no longer wedges the 64 KiB headless ring.
TEST_NAME := Job annotations
TEST_DESC := value/error annotation records in order under --framed; record bound on escaped output
TEST_ROM := roms/iix-iicx-se30-97221136.rom
TEST_RUNNER := run.sh

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
