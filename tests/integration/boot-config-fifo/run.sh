#!/bin/bash
# config= from a process substitution and a named FIFO (see config.mk).

set -e

# Set by the parent Makefile: HEADLESS_BIN, ROM_PATH, TEST_TMPDIR,
# STORAGE_CACHE.

mkdir -p "$TEST_TMPDIR"

# The Plus does not default to 1 MB, so 1024 means the document was read.
cat > "$TEST_TMPDIR/test.script" << 'EOF'
assert machine.ram == 1024 "the document's options.memory was not applied: ${machine.ram}"
echo "OK"
quit
EOF

# run LABEL ARGS...: boot with ARGS and expect the script's OK
run() {
    local label="$1"
    shift
    GS_STORAGE_CACHE="$STORAGE_CACHE" "$HEADLESS_BIN" rom="$ROM_PATH" "$@" \
        script="$TEST_TMPDIR/test.script" --speed=turbo > "$TEST_TMPDIR/$label.log" 2>&1 ||
        { cat "$TEST_TMPDIR/$label.log"; exit 1; }
    grep -q '^OK$' "$TEST_TMPDIR/$label.log" || { cat "$TEST_TMPDIR/$label.log"; exit 1; }
}

DOC='{"model":"plus","options":{"memory":"1024"}}'

# A regular file, the baseline
echo "$DOC" > "$TEST_TMPDIR/config.json"
run file config="$TEST_TMPDIR/config.json"

# Process substitution: a /dev/fd/N pipe
run procsub config=<(echo "$DOC")

# A named FIFO: the writer's open blocks until the emulator opens it to read
rm -f "$TEST_TMPDIR/config.fifo"
mkfifo "$TEST_TMPDIR/config.fifo"
echo "$DOC" > "$TEST_TMPDIR/config.fifo" &
run fifo config="$TEST_TMPDIR/config.fifo"
wait

echo "boot-config-fifo: config= reads a regular file, a process substitution and a named FIFO"
