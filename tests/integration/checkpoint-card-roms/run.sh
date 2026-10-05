#!/bin/bash
# Two-step card-ROM checkpoint test (see config.mk).

set -e

# Set by the parent Makefile: HEADLESS_BIN, ROM_PATH, TEST_DATA, TEST_TMPDIR

PM_ROM="$TEST_DATA/roms/pm7500-pm8500-pm9500-96cd923d.rom"
NUBUS_CP="$TEST_TMPDIR/iicx-cards.checkpoint"
PCI_CP="$TEST_TMPDIR/pm9500-mach64.checkpoint"

echo "Step 1: two 24ACs on an IIcx, a Mach64 in a 9500's slot 1; save both"
cat > "$TEST_TMPDIR/step1.script" << SCRIPT
machine.boot model="iicx" ram=8192 rom="$ROM_PATH" slots="9=display_card_24ac;10=display_card_24ac"
assert machine.nubus.slot[9].card.id == "display_card_24ac" "socket \$9's card"
assert machine.nubus.slot[10].card.id == "display_card_24ac" "socket \$A's card"
assert machine.nubus.slot[9].card.declrom.crc == 0xd8daab87 "socket \$9's declaration ROM"
assert machine.nubus.slot[10].card.declrom.crc == 0xd8daab87 "socket \$A's declaration ROM"
scheduler.run 3000000
assert checkpoint.save("$NUBUS_CP") "IIcx checkpoint save"

machine.boot model="pm9500" ram=32768 rom="$PM_ROM" slots="1=mach64_gx"
assert machine.pci.slot[1].card.id == "mach64_gx" "PCI slot 1's card"
assert machine.pci.slot[1].card.config.rom_size > 0 "the Mach64 has its FCode PROM"
scheduler.run 3000000
assert checkpoint.save("$PCI_CP") "9500 checkpoint save"
quit
SCRIPT

$HEADLESS_BIN rom="$ROM_PATH" script="$TEST_TMPDIR/step1.script" --speed=turbo

# A ROM directory with the machines' ROMs and no card ROM: nothing to offer.
BARE="$TEST_TMPDIR/bare"
mkdir -p "$BARE"
rm -f "$BARE"/*.rom
cp "$ROM_PATH" "$BARE/iicx.rom"
cp "$PM_ROM" "$BARE/pm9500.rom"

echo ""
echo "Step 2: restore both where no card ROM is offered"
cat > "$TEST_TMPDIR/step2.script" << SCRIPT
# Nothing offers the 24AC's declaration ROM here: a boot asking for the card
# gets its substitute ROM...
assert machine.boot(model="iicx", ram=8192, rom="$BARE/iicx.rom", slots="9=display_card_24ac") "the 24AC boots its substitute"
assert machine.nubus.slot[9].card.substitute "a 24AC found Apple's ROM with none offered"

assert checkpoint.load("$NUBUS_CP") "IIcx checkpoint load"
assert machine.id == "iicx" "restored model"
assert machine.nubus.slot[9].card.id == "display_card_24ac" "restored socket \$9's card"
assert machine.nubus.slot[10].card.id == "display_card_24ac" "restored socket \$A's card"
assert machine.nubus.slot[9].card.declrom.crc == 0xd8daab87 "restored socket \$9's declaration ROM"
assert machine.nubus.slot[10].card.declrom.crc == 0xd8daab87 "restored socket \$A's declaration ROM"
# ...while a checkpoint restores the ROM it carries: Apple's.
assert machine.nubus.slot[9].card.substitute == false "the restored card fell back to its substitute"
scheduler.run 3000000

assert checkpoint.load("$PCI_CP") "9500 checkpoint load"
assert machine.id == "pm9500" "restored model"
assert machine.pci.slot[1].card.id == "mach64_gx" "restored PCI slot 1's card"
assert machine.pci.slot[1].card.config.rom_size > 0 "the restored Mach64 has its FCode PROM"
scheduler.run 3000000
echo "checkpoint-card-roms ok"
quit
SCRIPT

$HEADLESS_BIN rom="$BARE/iicx.rom" script="$TEST_TMPDIR/step2.script" --speed=turbo | tee "$TEST_TMPDIR/step2.log"
grep -q "checkpoint-card-roms ok" "$TEST_TMPDIR/step2.log"

echo "Checkpoint card ROM test passed!"
