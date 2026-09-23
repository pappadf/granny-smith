#!/usr/bin/env bash
# Every class descriptor that reaches the object tree must pass
# object_validate_class (08-core-infra G2).
#
# The validator itself runs inside object_new() in non-GS_FAST builds and
# prints "object: class '<name>' invalid: <why>" on stderr.  Nothing made that
# an error, though, so a broken descriptor only ever produced a node that was
# quietly wrong -- machine.cpu.mmu and machine.cpu.fpu on every PowerPC machine
# came up with a NULL class name and no members, and no test noticed.
#
# So boot each machine and fail on any such line.  Booting is what makes this
# total: a class is only checked once something instantiates it, and only the
# machine that owns a device instantiates that device's classes.  A few hundred
# thousand cycles is enough -- every *_init has run by then, which is where the
# object tree is built.
#
# MODELS= is every entry in machine.c's builtin_machines[].  Unlike the
# hand-written assertions in machine-capabilities, adding a model here DOES buy
# coverage: the check is generic, so a new machine's classes are checked the
# moment its id appears below.
set -euo pipefail

SCRIPT="$WORK_DIR/boot.script"
mkdir -p "$WORK_DIR"
printf 'scheduler.run 200000\nquit\n' > "$SCRIPT"

# model:rom-under-tests/data/roms
MODELS="
plus:plus-v3-4d1f8172.rom
se30:iix-iicx-se30-97221136.rom
iix:iix-iicx-se30-97221136.rom
iicx:iix-iicx-se30-97221136.rom
iifx:iifx-4147dd77.rom
iici:iici-368cadfe.rom
iisi:iisi-36b7fb6c.rom
q700:q700-q900-420dbff3.rom
q900:q700-q900-420dbff3.rom
q950:q950-3dc27823.rom
q840av:q840av-q660av-5bf10fd1.rom
q660av:q840av-q660av-5bf10fd1.rom
pm6100:pm6100-pm7100-pm8100-9feb69b3.rom
pm7100:pm6100-pm7100-pm8100-9feb69b3.rom
pm8100:pm6100-pm7100-pm8100-9feb69b3.rom
pm7500:pm7500-pm8500-pm9500-96cd923d.rom
pm8500:pm7500-pm8500-pm9500-96cd923d.rom
pm9500:pm7500-pm8500-pm9500-96cd923d.rom
ans500:ans500-ans700-962f6c13.rom
ans700:ans500-ans700-962f6c13.rom
lisa:lisa2-revh-098917b2.rom
macxl:macxl-3a-094c82f0.rom
"

fail=0
n=0
for entry in $MODELS; do
    model="${entry%%:*}"
    rom="$TEST_DATA/roms/${entry##*:}"
    if [ ! -f "$rom" ]; then
        echo "FAIL: $model: ROM not found: $rom"
        fail=1
        continue
    fi
    out="$WORK_DIR/$model.log"
    # A model that cannot boot at all is a different test's problem, so the
    # exit status is not checked here -- only what the validator said.
    "$HEADLESS_BIN" rom="$rom" model="$model" script="$SCRIPT" --speed=max > "$out" 2>&1 || true
    n=$((n + 1))
    if grep -q '^object: class' "$out"; then
        echo "FAIL: $model registered an invalid class:"
        grep '^object: class' "$out" | sort -u | sed 's/^/    /'
        fail=1
    fi
done

if [ "$fail" -ne 0 ]; then
    echo ""
    echo "A class descriptor reaching object_new() failed object_validate_class."
    echo "See object.c's validator for the rule each message names."
    exit 1
fi

echo "$n model(s) booted; every registered class passed object_validate_class"
