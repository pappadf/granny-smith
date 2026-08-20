#!/usr/bin/env bash
# machine.profile() capability-probe assertions.
#
# Runs the headless shell once, dumps machine.profile for the models listed in
# MODELS= below, then greps each model's JSON line for the expected capability
# fields.  Each model's profile is a single JSON line containing
# "id":"<model>", so we isolate a model's line by that key.
#
# MODELS= is deliberately NOT "every registered model": every assertion here is
# hand-written and names its model explicitly, so adding a model to the list
# alone buys no coverage — it just dumps a JSON line nothing inspects.  A new
# machine earns coverage here only by having assertions written for it.  (The
# shape of the profile JSON *is* checked for every registered model, by the
# sibling machine-profile-schema test.)
set -euo pipefail

OUT="$WORK_DIR/profiles.txt"
SCRIPT="$WORK_DIR/profiles.script"
mkdir -p "$WORK_DIR"

MODELS="plus se30 iicx iix iifx iici iisi q840av q660av pm6100 pm7100 pm8100 lisa macxl"

: > "$SCRIPT"
for m in $MODELS; do
    echo "echo \"\${machine.profile(\"$m\")}\"" >> "$SCRIPT"
done
echo "quit" >> "$SCRIPT"

"$HEADLESS_BIN" rom="$ROM_PATH" script="$SCRIPT" --speed=max > "$OUT" 2>&1

fail=0

# Return the single JSON line for a model id.
profile_line() {
    grep "\"id\":\"$1\"" "$OUT" | head -1
}

# assert_contains <model> <needle> <description>
assert_contains() {
    local model="$1" needle="$2" desc="$3"
    local line
    line=$(profile_line "$model")
    if [ -z "$line" ]; then
        echo "FAIL: no profile JSON for model '$model'"
        fail=1
        return
    fi
    if ! printf '%s' "$line" | grep -qF "$needle"; then
        echo "FAIL: $model: expected $desc ($needle)"
        fail=1
    fi
}

# assert_absent <model> <needle> <description>
assert_absent() {
    local model="$1" needle="$2" desc="$3"
    local line
    line=$(profile_line "$model")
    if printf '%s' "$line" | grep -qF "$needle"; then
        echo "FAIL: $model: unexpected $desc ($needle)"
        fail=1
    fi
}

# --- MMU capability kind, per model -------------------------------------
assert_contains plus  '"kind":"none"'         "mmu kind none"
assert_contains se30  '"kind":"68030_pmmu"'   "mmu kind 68030_pmmu"
assert_contains iicx  '"kind":"68030_pmmu"'   "mmu kind 68030_pmmu"
assert_contains iix   '"kind":"68030_pmmu"'   "mmu kind 68030_pmmu"
assert_contains iifx  '"kind":"68030_pmmu"'   "mmu kind 68030_pmmu"
assert_contains iici  '"kind":"68030_pmmu"'   "mmu kind 68030_pmmu"
assert_contains iisi  '"kind":"68030_pmmu"'   "mmu kind 68030_pmmu"
assert_contains lisa  '"kind":"lisa_segment"' "mmu kind lisa_segment"
assert_contains macxl '"kind":"lisa_segment"' "mmu kind lisa_segment"

# --- CPU model + FPU derivation -----------------------------------------
assert_contains plus '"model":68000'  "cpu model 68000"
assert_contains plus '"fpu":false'    "no fpu on 68000"
assert_contains se30 '"model":68030'  "cpu model 68030"
assert_contains se30 '"fpu":true'     "fpu on 68030"

# --- nubus capability ----------------------------------------------------
assert_contains plus '"nubus":false'  "plus has no nubus"
assert_contains iicx '"nubus":true'   "iicx has nubus"

# --- VROM-by-card: the SE/30-vs-IIci asymmetry --------------------------
# IIcx user video slot offers the 8·24 card, which requires a VROM file.
assert_contains iicx '"id":"mdc_8_24"'        "iicx 8·24 video card"
assert_contains iicx '"requires_vrom":true'   "iicx card needs vrom"
# IIci built-in RBV video carries its declaration in main ROM — no VROM.
# (Match the whole card object: since stage 2 the IIci also declares three
# sockets whose pluggable candidates DO need a vROM, so a profile-wide
# requires_vrom:true absence check would be wrong.)
assert_contains iici '"id":"builtin_rbv_video","display_name":"Macintosh IIci Built-in Video","requires_vrom":false' \
    "iici builtin video needs no vrom"

# --- Computed card compatibility (no per-machine whitelists) --------------
# Socket candidates are computed from the card registry by attachment
# (nubus_card_fits_socket): every CARD_ATTACH_NUBUS video card is offered on
# every machine with a user-configurable socket — including the IIci's three
# empty sockets next to its builtin video
# (proposal-nubus-computed-card-compatibility.md §5.3).
for m in iicx iix iifx iici; do
    assert_contains "$m" '"id":"mdc_8_24"'          "$m offers 8·24"
    assert_contains "$m" '"id":"display_card_24ac"' "$m offers 24AC"
    assert_contains "$m" '"id":"824gc"'             "$m offers 8·24 GC"
done
# Stage 2: machines declare EVERY socket (topology), not just one video
# slot — the IIcx's three, the IIx/IIfx's six, the IIci's three.
for m in iicx iix iifx; do
    assert_contains "$m" '"slot":"9"' "$m declares socket \$9"
    assert_contains "$m" '"slot":"A"' "$m declares socket \$A"
    assert_contains "$m" '"slot":"B"' "$m declares socket \$B"
done
assert_contains iix  '"slot":"E"' "iix declares socket \$E"
assert_contains iifx '"slot":"E"' "iifx declares socket \$E"
assert_contains iici '"slot":"C"' "iici declares socket \$C"
assert_contains iici '"slot":"E"' "iici declares socket \$E"
# The attach gate: builtin pseudo-cards (motherboard circuitry impersonating
# a slot device) must never be offered on a socket — only where a BUILTIN
# slot decl names them.
for m in iicx iix iifx; do
    assert_absent "$m" '"id":"builtin_se30_video"' "$m must not offer the SE/30 builtin"
    assert_absent "$m" '"id":"builtin_rbv_video"'  "$m must not offer the RBV builtin"
done
assert_absent iici '"id":"builtin_se30_video"' "iici must not offer the SE/30 builtin"
# Conversely a machine with no socket offers no pluggable cards: the SE/30's
# $9..$B are decoded-but-connectorless (EMPTY), so only its builtin appears.
assert_contains se30 '"id":"builtin_se30_video"' "se30 builtin video card"
for card in mdc_8_24 display_card_24ac 824gc; do
    assert_absent se30 "\"id\":\"$card\"" "se30 has no socket for $card"
    assert_absent iisi "\"id\":\"$card\"" "iisi has no socket for $card"
done

# --- The AV family (Quadra 840AV / Centris 660AV) --------------------------
# Both are 68040 machines with the integrated 040 MMU, and both carry the
# same 2 MB ROM — so what distinguishes them in the profile is the clock and
# the NuBus story.  The 840AV's three slots ride a MUNI bridge and the
# 660AV's single slot rides an adapter that is absent by default; neither
# machine declares NuBus sockets while no AV declaration-ROM work exists, so
# `nubus:false` here is the load-bearing assertion that the profile is not
# quietly offering cards the family cannot seat.
for m in q840av q660av; do
    assert_contains "$m" '"model":68040' "$m is a 68040"
    assert_contains "$m" '"kind":"68040"' "$m has the integrated 040 MMU"
    assert_contains "$m" '"fpu":true' "$m has an FPU"
    assert_contains "$m" '"address_bits":32' "$m is 32-bit"
    assert_contains "$m" '"nubus":false' "$m declares no NuBus sockets"
    assert_contains "$m" '"has_cdrom":true' "$m offers a CD-ROM bay"
    # The New Age FDC is stubbed as 'no drive', so no floppy slot is offered.
    assert_contains "$m" '"floppy_slots":[]' "$m offers no floppy drive"
done
# --- PDM family (Power Macintosh 6100/7100/8100): the first PowerPC
# machines.  cpu.model 601 + the 601 MMU kind are what gate the PPC debug
# panels; fpu:true since Phase E landed the 601 FPU datapath and the
# machine.cpu.fpu object (proposal-powerpc-601-pdm.md §3.6).  Phase G
# landed the Curio SCSI bus, so two HD slots AND the CD bay are offered —
# a CD-ROM is an ordinary SCSI target on that same bus, with no
# CD-specific hardware behind it.  ONE floppy slot, not two: the family
# has a single internal manual-inject SuperDrive and no external port, and
# this assertion moved only after a 1.44 MB disk mounted in the Finder on
# a booted 7100 and a PowerPC application launched off it (suite-pdm rows
# pdm-floppy-mount / pdm-floppy-boot) — a modelled drive is not
# evidence the guest can use it.
for m in pm6100 pm7100 pm8100; do
    assert_contains "$m" '"model":601' "$m is a PowerPC 601"
    assert_contains "$m" '"kind":"ppc_601"' "$m has the 601 BAT/segment/HTAB MMU"
    assert_contains "$m" '"fpu":true' "$m FPU capability on since Phase E"
    assert_contains "$m" '"address_bits":32' "$m is 32-bit"
    assert_contains "$m" '"has_cdrom":true' "$m offers the Curio-bus CD bay"
    assert_contains "$m" '"cdrom_id":3' "$m puts the CD at SCSI ID 3"
    assert_contains "$m" '"floppy_slots":[{"label":"Internal FD0","kind":"hd"}]' "$m offers the one internal SuperDrive"
    assert_contains "$m" '"scsi_slots":[{"label":"SCSI HD0","id":0},{"label":"SCSI HD1","id":1}]' "$m offers the two Curio SCSI HD slots (Phase G)"
done
# NuBus splits the family in two, and that split is the point of these
# rows.  The 7100 and 8100 carry BART and three connectors on the logic
# board; the 6100's bridge ships on an optional PDS adapter card that is
# not modeled, so it has no sockets at all — the ROM's own probe faults
# and clears BARTExists, which suite-pdm asserts from guest memory.  These
# assertions were flipped only after a pm8100 booted 7.5 with a 24AC in
# slot $C and the OS ran the card's driver as a second screen
# (suite-pdm row 8100-75-24ac); seating a card in the model is not
# evidence the guest can use it.
assert_contains pm6100 '"nubus":false' "pm6100 has no NuBus without the PDS adapter"
for m in pm7100 pm8100; do
    assert_contains "$m" '"nubus":true' "$m has the three BART NuBus sockets"
    # $B/$C/$D — NOT the widely repeated $C/$D/$E: slot $E is the PDS
    # video pseudo-slot on these boards, which is not a NuBus connector.
    assert_contains "$m" '"slot":"B"' "$m declares socket \$B"
    assert_contains "$m" '"slot":"C"' "$m declares socket \$C"
    assert_contains "$m" '"slot":"D"' "$m declares socket \$D"
    assert_absent "$m" '"slot":"E"' "$m must not offer the PDS pseudo-slot as a NuBus socket"
    # Computed compatibility again: every NuBus-attach video card is
    # offered on every socket, with no per-machine whitelist anywhere.
    assert_contains "$m" '"id":"mdc_8_24"' "$m offers 8·24"
    assert_contains "$m" '"id":"display_card_24ac"' "$m offers 24AC"
    assert_contains "$m" '"id":"824gc"' "$m offers 8·24 GC"
done
for card in mdc_8_24 display_card_24ac 824gc; do
    assert_absent pm6100 "\"id\":\"$card\"" "pm6100 has no socket for $card"
done
assert_contains pm6100 '"freq":60000000' "pm6100 runs at 60 MHz"
assert_contains pm7100 '"freq":66000000' "pm7100 runs at 66 MHz"
assert_contains pm8100 '"freq":80000000' "pm8100 runs at 80 MHz"

assert_contains q840av '"freq":40000000' "q840av runs at 40 MHz"
assert_contains q660av '"freq":25000000' "q660av runs at 25 MHz"
# The on-board DMSD/VDC video digitizer is what makes these machines "AV":
# video_in gates the frontend's camera control, and only this family has it.
for m in q840av q660av; do
    assert_contains "$m" '"video_in":true' "$m has the on-board video digitizer"
done
for m in plus se30 iicx iix iifx iici iisi lisa macxl; do
    assert_contains "$m" '"video_in":false' "$m has no video digitizer"
done
# Built-in CIVIC video is motherboard circuitry, not a card: neither machine
# may offer a pluggable video card.
for m in q840av q660av; do
    for card in mdc_8_24 display_card_24ac 824gc; do
        assert_absent "$m" "\"id\":\"$card\"" "$m has no socket for $card"
    done
done

if [ "$fail" -ne 0 ]; then
    echo "--- captured profile output ---"
    cat "$OUT"
    exit 1
fi

echo "machine-capabilities: all capability assertions passed"
