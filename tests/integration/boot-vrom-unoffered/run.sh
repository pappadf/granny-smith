#!/bin/bash
# vrom= without offers (see config.mk).

set -e

# Set by the parent Makefile: HEADLESS_BIN, TEST_DATA, TEST_TMPDIR,
# STORAGE_CACHE.

mkdir -p "$TEST_TMPDIR/rom" "$TEST_TMPDIR/vrom"
cp "$TEST_DATA/roms/iix-iicx-se30-97221136.rom" "$TEST_TMPDIR/rom/"
cp "$TEST_DATA/roms/display-card-24ac-d8daab87.vrom" "$TEST_TMPDIR/vrom/"
ROM="$TEST_TMPDIR/rom/iix-iicx-se30-97221136.rom"
VROM="$TEST_TMPDIR/vrom/display-card-24ac-d8daab87.vrom"

cat > "$TEST_TMPDIR/test.script" << EOF
# Nothing offered: a card with both ROMs is listed only on its generic one,
# and the default document seats that (#346).
let ids = ""
for c in catalog.profile("iicx").cards {
    \$ids = "\${\$ids} \${\$c.id}"
}
assert "\$ids" == " mdc_8_24_generic display_card_24ac_generic 824gc_generic" "only the generic entries without ROMs: \$ids"
assert catalog.default_config("iicx").cards[0].card == "mdc_8_24_generic" "the default card is not the generic 8•24"
assert catalog.default_config("se30").options.video_rom == "generic" "the SE/30 defaults to a real video ROM it does not have"
assert machine.boot(model="iicx", rom="$ROM", config="{\"cards\":[{\"slot\":\"nubus_9\",\"card\":\"mdc_8_24\"}]}") "a document naming the real 8•24 without its ROM boots"
assert machine.nubus.slot[9].card.substitute "the real 8•24 without its ROM does not fall back to the generic one"
# Nothing offered: the named card has no ROM of Apple's, so it boots its
# substitute.
assert machine.boot(model="iicx", rom="$ROM", video_card="display_card_24ac") "a named card with no ROM offered boots its substitute"
assert machine.nubus.slot[9].card.substitute "the card runs its substitute ROM"
assert machine.nubus.slot[9].card.declrom.path == "builtin:display_card_24ac" "the generated declaration ROM"
# The document's vrom= supplies Apple's.
assert machine.boot(model="iicx", rom="$ROM", video_card="display_card_24ac", vrom="$VROM") "vrom= must satisfy the named card"
assert machine.nubus.slot[9].card.name == "Macintosh Display Card 24AC" "the named card is seated"
assert machine.nubus.slot[9].card.declrom.path == "$VROM" "the card runs the document's vrom="
assert machine.nubus.slot[9].card.substitute == false "Apple's ROM, not the substitute"
# vrom= never chooses a card: without video_card= the IIcx's socket holds its
# default, the 8.24, which this file is not for, so the boot is refused.
assert !machine.boot(model="iicx", rom="$ROM", vrom="$VROM") "a vrom= for a card no slot holds must be rejected"
echo "OK"
quit
EOF

GS_STORAGE_CACHE="$STORAGE_CACHE" $HEADLESS_BIN rom="$ROM" model=iicx script="$TEST_TMPDIR/test.script" \
    --speed=turbo > "$TEST_TMPDIR/out.log" 2>&1 || { cat "$TEST_TMPDIR/out.log"; exit 1; }
grep -q '^OK$' "$TEST_TMPDIR/out.log" || { cat "$TEST_TMPDIR/out.log"; exit 1; }
echo "boot-vrom-unoffered: with nothing offered a named card boots its substitute ROM, and the document's vrom= Apple's"
