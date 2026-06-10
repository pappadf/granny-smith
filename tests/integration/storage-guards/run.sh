#!/bin/bash
# storage.rm / storage.mv guard probes. Each refusal returns a V_ERROR,
# which (by design) makes the headless script exit non-zero — so this
# runner matches the refusal messages and verifies side effects itself
# instead of using the standard exit-code-only pattern.

# Set by the parent Makefile: HEADLESS_BIN, ROM_PATH, WORK_DIR

set -u

PROBE="$WORK_DIR/mvprobe"
PROBE2="$WORK_DIR/mvprobe2"
PROBE3="$WORK_DIR/mvprobe3"
mkdir -p "$PROBE" "$PROBE2"

OUT=$("$HEADLESS_BIN" rom="$ROM_PATH" --speed=max -q --script-stdin --no-prompt <<SCRIPT 2>&1
storage.rm /opfs
storage.rm /
storage.mv /opfs /tmp/gs-guards-relocated
storage.mv $PROBE $PROBE/sub
storage.mv $PROBE $PROBE2
storage.mv $PROBE $PROBE3
quit
SCRIPT
)
echo "$OUT"

fail=0
expect() {
  if ! grep -qF "$1" <<<"$OUT"; then
    echo "MISSING expected output: $1"
    fail=1
  fi
}

expect "storage.rm: refusing to remove '/opfs'"
expect "storage.rm: refusing to remove '/'"
expect "storage.mv: refusing to move '/opfs'"
expect "storage.mv: cannot move '$PROBE' into itself"
expect "storage.mv: destination '$PROBE2' already exists"

# Side effects: the guards must not have created or moved anything; the
# final (legitimate) move must have relocated the probe dir.
[ ! -e /tmp/gs-guards-relocated ] || { echo "FAIL: /opfs relocation side effect"; fail=1; }
[ -d "$PROBE2" ] || { echo "FAIL: existing destination was clobbered"; fail=1; }
[ -d "$PROBE3" ] || { echo "FAIL: legitimate move did not happen"; fail=1; }
[ ! -e "$PROBE" ] || { echo "FAIL: legitimate move left the source behind"; fail=1; }

exit $fail
