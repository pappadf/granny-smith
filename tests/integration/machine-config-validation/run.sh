#!/usr/bin/env bash
# machine.boot config= validation: one headless run per case, the rejection
# message matched exactly (docs/reference/object-model.md, the boot table).
set -euo pipefail

mkdir -p "$WORK_DIR"
ROMS="$TEST_DATA/roms"
IICI="$ROMS/iici-368cadfe.rom"
IIX="$ROMS/iix-iicx-se30-97221136.rom"
Q900="$ROMS/q700-q900-420dbff3.rom"
TNT="$ROMS/pm7500-pm8500-pm9500-96cd923d.rom"
G3="$ROMS/pmg3dt-pmg3mt-78f57389.rom"
ANS="$ROMS/ans500-ans700-962f6c13.rom"

fail=0
n=0

# boot_case <name> <model> <rom> <config JSON> <expected message | OK>
boot_case() {
    local name=$1 model=$2 rom=$3 config=$4 want=$5
    local script="$WORK_DIR/$name.script" out="$WORK_DIR/$name.out"
    local escaped=${config//\\/\\\\}
    escaped=${escaped//\"/\\\"}
    printf 'machine.boot model="%s" rom="%s" config="%s"\necho "BOOTED ${machine.id}"\nquit\n' \
        "$model" "$rom" "$escaped" > "$script"
    "$HEADLESS_BIN" rom="$IICI" script="$script" --speed=turbo > "$out" 2>&1 || true
    n=$((n + 1))
    if [ "$want" = OK ]; then
        if grep -q "^BOOTED $model\$" "$out"; then return; fi
        echo "FAIL: $name: expected the boot to succeed"
    else
        if grep -qF "machine.boot: $want" "$out" && ! grep -q '^BOOTED' "$out"; then return; fi
        echo "FAIL: $name: expected \"$want\""
    fi
    sed -n '/running/,$p' "$out" | head -5
    fail=1
}

# The document as JSON, and the core's parser.
boot_case json iici "$IICI" '{"storage": [}' 'config: JSON: expected a value at offset 13'
boot_case key iici "$IICI" '{"bogus": 1}' "config: unknown member 'bogus'"
boot_case model iici "$IICI" '{"model": "iix"}' "model= and config's model disagree"

# V1: option values, and only the model's options.
boot_case v1-value iici "$IICI" '{"options":{"memory":"12345"}}' \
    'options.memory: "12345" is not one of the model'"'"'s memory sizes'
boot_case v1-option iici "$IICI" '{"options":{"bogus":"x"}}' "options: model 'iici' has no option 'bogus'"

# V2: a floppy position takes its own drive types.
boot_case v2 iici "$IICI" '{"floppies":{"fd0":"800k"}}' 'floppies.fd0: "800k" is not a drive type this position takes'

# V3: a bus the model has, a unit it has, one ID space across connectors,
# a device type the bus takes.
boot_case v3-bus iici "$IICI" '{"storage":[{"bus":"nope","unit":0,"type":"hd"}]}' \
    "storage[0]: model 'iici' has no bus 'nope'"
boot_case v3-unit iici "$IICI" '{"storage":[{"bus":"scsi","unit":7,"type":"hd"}]}' 'storage[0]: "SCSI" has no unit 7'
boot_case v3-shared q900 "$Q900" \
    '{"storage":[{"bus":"scsi","unit":1,"type":"hd"},{"bus":"scsi2","unit":1,"type":"hd"}]}' \
    'storage[1]: ID 1 · External on "External SCSI" is already used on "Internal SCSI" (shared ID space)'
boot_case v3-type pmg3dt "$G3" '{"storage":[{"bus":"ata0","unit":0,"type":"cd"}]}' \
    'storage[0]: "Primary ATA bus" takes no CD-ROM drive'

# V4: a slot the model has, a card that fits it, one card per slot.
boot_case v4-slot iici "$IICI" '{"cards":[{"slot":"nubus_9","card":"mdc_8_24"}]}' \
    "cards[0]: model 'iici' has no slot 'nubus_9'"
boot_case v4-twice iici "$IICI" \
    '{"cards":[{"slot":"nubus_c","card":"mdc_8_24"},{"slot":"nubus_c","card":"824gc"}]}' \
    'cards[1]: "NuBus slot 4" is already occupied'
boot_case v4-fit pm7500 "$TNT" '{"cards":[{"slot":"pci_1","card":"mdc_8_24"}]}' \
    'cards[0]: "Macintosh Display Card 8•24" does not fit "PCI slot A1"'
boot_case v4-card iici "$IICI" '{"cards":[{"slot":"nubus_c","card":"nope"}]}' "cards[0]: no card 'nope'"

# V5: a declared card option takes its declared values.
boot_case v5 pm7500 "$TNT" '{"cards":[{"slot":"pci_1","card":"mach64_gx","options":{"vram":"3m"}}]}' \
    'cards[0].options.vram: "3m" is not one of the card'"'"'s values'

# V7: display devices of this configuration, their monitors and modes, and
# one connected monitor.
boot_case v7-two iici "$IICI" \
    '{"cards":[{"slot":"nubus_c","card":"mdc_8_24"}],"displays":{"builtin":{"monitor":"13in_rgb"},"nubus_c":{"monitor":"13in_rgb"}}}' \
    'displays: 2 monitors connected; this build supports 1'
boot_case v7-monitor iici "$IICI" '{"displays":{"builtin":{"monitor":"21in_rgb"}}}' \
    'displays.builtin: monitor "21in_rgb" is not one this device takes'
boot_case v7-device iici "$IICI" '{"displays":{"nubus_d":{"monitor":"13in_rgb"}}}' \
    'displays: "nubus_d" is not a display device of this configuration'
boot_case v7-mode iici "$IICI" \
    '{"cards":[{"slot":"nubus_c","card":"mdc_8_24"}],"displays":{"nubus_c":{"monitor":"13in_rgb","mode":"1152x870x8"}}}' \
    'displays.nubus_c: mode "1152x870x8" is not one of'
boot_case v7-builtin-mode iici "$IICI" '{"displays":{"builtin":{"monitor":"13in_rgb","mode":"640x480x8"}}}' OK
boot_case v7-builtin-depth iici "$IICI" '{"displays":{"builtin":{"monitor":"13in_rgb","mode":"640x480x16"}}}' \
    'displays.builtin: mode "640x480x16" is not one of the built-in video'"'"'s modes on that monitor'

# V9: the startup device is in storage, on a bus the startup record can name.
boot_case v9-absent iici "$IICI" '{"startup":{"bus":"scsi","unit":5}}' 'startup: no device at scsi unit 5 in storage'
boot_case v9-bus ans500 "$ANS" \
    '{"storage":[{"bus":"scsi2","unit":4,"type":"hd"}],"startup":{"bus":"scsi2","unit":4}}' \
    'startup: ID 4 · Front bay 4 on "Internal SCSI bus 1" — this machine'"'"'s startup record cannot name that bus'

# The valid twins.
boot_case ok-card iici "$IICI" \
    '{"cards":[{"slot":"nubus_c","card":"mdc_8_24"}],"displays":{"nubus_c":{"monitor":"13in_rgb","mode":"640x480x8"}}}' OK
boot_case ok-shared q900 "$Q900" \
    '{"storage":[{"bus":"scsi","unit":1,"type":"hd"},{"bus":"scsi2","unit":2,"type":"hd"}],"startup":null}' OK
boot_case ok-empty iici "$IICI" '{"storage":[],"cards":[],"startup":null}' OK

# V8, which needs a legacy argument beside the document.
script="$WORK_DIR/v8.script"
printf 'machine.boot model="iici" rom="%s" video_card="mdc_8_24" config="{\\"cards\\":[]}"\nquit\n' "$IICI" > "$script"
"$HEADLESS_BIN" rom="$IICI" script="$script" --speed=turbo > "$WORK_DIR/v8.out" 2>&1 || true
n=$((n + 1))
if ! grep -qF "machine.boot: config's cards and the legacy card arguments" "$WORK_DIR/v8.out"; then
    echo "FAIL: v8: expected the mixing to be refused"
    fail=1
fi

if [ "$fail" -ne 0 ]; then
    exit 1
fi
echo "machine-config-validation: $n cases"
