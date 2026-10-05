#!/usr/bin/env bash
# Removal inventory (see config.mk): each row names what replaced it.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
fail=0

# absent <what replaced it> <extended regex> [paths...]
absent() {
    local why=$1 pattern=$2
    shift 2
    local hits
    hits=$(cd "$ROOT" && grep -rnE --include='*.c' --include='*.h' --include='*.ts' --include='*.svelte' \
        "$pattern" "$@" 2>/dev/null || true)
    if [ -n "$hits" ]; then
        echo "BACK: /$pattern/ (replaced by $why):"
        echo "$hits" | sed 's/^/    /'
        fail=1
    fi
}

SRC="src app/web2/src"

# The screen is the device the monitor is connected to, not a family rule.
absent "the connected display device" 'nubus_primary_display|pci_primary_display' $SRC
# Built-in video is one display device; a card never replaces it.
absent "the built-in video's own ROM choice" 'video_card_pick_fits' $SRC
# One card kind per card, its substitute ROM inside.
absent "a card kind's substitute ROM" '_generic_kind|substitute_for|card_init_generic|card_name_generic' $SRC
absent "a card kind's substitute ROM" '\.id = "(8_24|24ac|8_24gc|se30)"' src/core/peripherals/nubus src/machines/glue/builtin_se30_video.c
absent "a card kind's substitute ROM" 'synthesise_vrom_fallback' $SRC
# Per-device monitor sense.
absent "per-device sense in the build options" \
    'MACHINE_SENSE_UNSET|machine_sense_or|machine_slot_sense|build_opts\.video_sense|dafb_sense_for_build|pdm_monitor_for_build|rbv_set_monitor_sense' $SRC
# Cards produce their startup record; the seeding step writes PRAM.
absent "the seeding step" 'rtc_pram_write|system_rtc' src/core/peripherals/nubus/cards
# Construction arguments, not staging globals.
absent "the build options" 'nubus_staged_|pci_staged_|STAGED_WILDCARD|s_staged\[' $SRC
# The configuration names the startup device; the core seeds it.
absent "the document's startup device" 'setStartupDisk' $SRC
# Images attach to a device by position.
absent "attach by device position" 'hdBay' $SRC
# No machine is the default by name.
absent "an explicit model" 'machine_find\("plus"\)' $SRC
# The tree replaced the v1 profile keys.
absent "the machine-description tree" '"(ram_options|video_slots|hd_bays|scsi_buses|floppy_slots)"' src/machines src/core

if [ "$fail" -ne 0 ]; then
    echo "removal-inventory: FAILED"
    exit 1
fi
echo "removal-inventory: OK"
