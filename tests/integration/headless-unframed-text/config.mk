# Integration test: unframed headless prints a job's text and its error
# lines byte-exactly.
#
# Unframed, headless decodes the JSON string of every `output` record (to
# stdout) and of an `error` record's `lines` (to stderr).  The decoder used
# to know only \n, \t and \u, so the \r, \b and \f short forms the core's
# escaper writes printed as the letters r, b and f: an assert message with a
# carriage return reached stderr with a stray `r`.
TEST_NAME := Headless unframed text
TEST_DESC := job output and error lines with CR/BS/FF/quote/backslash/non-ASCII reach stdout/stderr byte-exact
TEST_ROM := roms/plus-v3-4d1f8172.rom
TEST_RUNNER := run.sh

# CI tier (docs/guide/TESTING.md, "Tiers"): unit | matrix | extended
TEST_TIER := unit
