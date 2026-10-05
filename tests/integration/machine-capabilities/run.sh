#!/usr/bin/env bash
# catalog.profile() capability-probe assertions.
#
# Runs the headless shell once, dumps catalog.profile for the models listed in
# MODELS= below, then greps each model's JSON line for the expected capability
# fields.  Each model's profile is a single JSON line containing
# "id":"<model>", so we isolate a model's line by that key.
#
# Every assertion here is hand-written and names its model, so listing a model
# buys no coverage by itself.  What the test does enforce is a floor: it dumps
# every registered model (catalog.models) and fails if any has no assertion at
# all -- which is how the three MCU Quadras went uncovered here while the
# sibling machine-profile-schema test already carried them.
set -euo pipefail

OUT="$WORK_DIR/profiles.txt"
SCRIPT="$WORK_DIR/profiles.script"
mkdir -p "$WORK_DIR"

# Every registered model, read from the emulator itself (catalog.models),
# so a newly registered machine cannot be silently left out.
cat > "$SCRIPT" <<'SCRIPT'
let ms = catalog.models
for m in $ms {
    echo "${catalog.profile($m)}"
}
quit
SCRIPT

"$HEADLESS_BIN" rom="$ROM_PATH" script="$SCRIPT" --speed=turbo > "$OUT" 2>&1

fail=0
declare -A count=()

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
    count[$model]=$(( ${count[$model]:-0} + 1 ))
    # A here-string, not printf | grep -q: under pipefail, grep -q exiting at
    # the first match can SIGPIPE the printf and fail a passing assertion.
    if ! grep -qF -- "$needle" <<<"$line"; then
        echo "FAIL: $model: expected $desc ($needle)"
        fail=1
    fi
}

# assert_not_contains <model> <needle> <description>
assert_not_contains() {
    local model="$1" needle="$2" desc="$3"
    local line
    line=$(profile_line "$model")
    if [ -z "$line" ]; then
        echo "FAIL: no profile JSON for model '$model'"
        fail=1
        return
    fi
    count[$model]=$(( ${count[$model]:-0} + 1 ))
    if grep -qF -- "$needle" <<<"$line"; then
        echo "FAIL: $model: expected NOT $desc ($needle)"
        fail=1
    fi
}

# assert_absent <model> <needle> <description>
assert_absent() {
    local model="$1" needle="$2" desc="$3"
    local line
    line=$(profile_line "$model")
    count[$model]=$(( ${count[$model]:-0} + 1 ))
    if grep -qF -- "$needle" <<<"$line"; then
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

# --- Card ROMs: a card names its declaration ROM and whether the emulator
# can stand in for it; built-in video is a display device, not a card.
assert_contains iicx '"id":"mdc_8_24","label":"Macintosh Display Card 8•24","class":"display"' "iicx 8·24 video card"
assert_contains iicx '"rom":{"kind":"vrom","substitute":true}' "iicx card has a vROM with a substitute"
assert_contains iici '"builtin":{"id":"builtin","label":"Built-in video","detail":"RBV"' "iici built-in RBV video"
assert_absent iici '"id":"builtin_rbv_video"' "iici built-in video is not a card"

# --- Computed card compatibility (no per-machine whitelists) --------------
# Socket candidates are computed from the card registry by attachment
# (nubus_card_fits_socket): every CARD_ATTACH_NUBUS video card is offered on
# every machine with a user-configurable socket — including the IIci's three
# empty sockets next to its builtin video (docs/guide/ARCHITECTURE.md,
# "Computed card compatibility").
for m in iicx iix iifx iici; do
    assert_contains "$m" '"id":"mdc_8_24"'          "$m offers 8·24"
    assert_contains "$m" '"id":"display_card_24ac"' "$m offers 24AC"
    assert_contains "$m" '"id":"824gc"'             "$m offers 8·24 GC"
done
# Machines declare EVERY socket (topology), named as Apple names them on
# that machine (D12) with the slot ID as detail -- the IIcx's three, the
# IIx/IIfx's six, the IIci's three.
for m in iicx iix iifx; do
    assert_contains "$m" '{"id":"nubus_9","label":"NuBus slot 1","detail":"Slot ID $9"' "$m declares socket \$9"
    assert_contains "$m" '{"id":"nubus_a","label":"NuBus slot 2"' "$m declares socket \$A"
    assert_contains "$m" '{"id":"nubus_b","label":"NuBus slot 3"' "$m declares socket \$B"
done
assert_contains iix  '{"id":"nubus_e","label":"NuBus slot 6"' "iix declares socket \$E"
assert_contains iifx '{"id":"nubus_e","label":"NuBus slot 6"' "iifx declares socket \$E"
assert_contains iici '{"id":"nubus_c","label":"NuBus slot 4","detail":"Slot ID $C"' "iici declares socket \$C"
assert_contains iici '{"id":"nubus_e","label":"NuBus slot 6"' "iici declares socket \$E"
# The default display card of a machine without built-in video (D17).
for m in iicx iix iifx; do
    assert_contains "$m" '"cards":[{"slot":"nubus_9","card":"mdc_8_24","options":{}}]' "$m opens with the 8·24 in its first slot"
done
# The attach gate: builtin pseudo-cards (motherboard circuitry impersonating
# a slot device) must never be offered on a socket — only where a BUILTIN
# slot decl names them.
for m in iicx iix iifx; do
    assert_absent "$m" '"id":"builtin_se30_video"' "$m must not offer the SE/30 builtin"
    assert_absent "$m" '"id":"builtin_rbv_video"'  "$m must not offer the RBV builtin"
done
assert_absent iici '"id":"builtin_se30_video"' "iici must not offer the SE/30 builtin"
# Conversely a machine with no socket offers no pluggable cards: the SE/30's
# $9..$B are decoded-but-connectorless (EMPTY), so only its built-in video
# (the slot-$E pseudo-card) appears, as a display device.
assert_contains se30 '"builtin":{"id":"builtin","label":"Built-in video","detail":"SE/30 video, slot $E","monitors":["compact_9in"]' "se30 built-in video"
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
    assert_contains "$m" '{"unit":3,"label":"ID 3 · CD-ROM bay"}' "$m has a CD-ROM bay at ID 3"
    assert_contains "$m" '{"bus":"scsi","unit":3,"type":"cd"}' "$m opens with a CD-ROM drive in it"
    # A GAP, pinned so that closing it shows up here: the real machines have
    # a SuperDrive, but the New Age FDC is not modeled (a 'no drive' stub),
    # so the profile offers no floppy position (#178).
    assert_contains "$m" '"floppies":[]' "$m offers no floppy position (New Age unmodeled, #178)"
done
# --- PDM family (Power Macintosh 6100/7100/8100): the first PowerPC
# machines.  cpu.model 601 + the 601 MMU kind are what gate the PPC debug
# panels; fpu:true since the 601 FPU datapath and the machine.cpu.fpu
# object landed.  The Curio SCSI bus is modelled, so two HD slots AND the
# CD bay are offered — a CD-ROM is an ordinary SCSI target on that same
# bus, with no CD-specific hardware behind it.  ONE floppy slot, not two: the family
# has a single internal manual-inject SuperDrive and no external port, and
# this assertion moved only after a 1.44 MB disk mounted in the Finder on
# a booted 7100 and a PowerPC application launched off it (suite-pdm rows
# pdm-floppy-mount / pdm-floppy-boot) — a modelled drive is not
# evidence the guest can use it.
for m in pm6100 pm7100 pm8100; do
    assert_contains "$m" '"model":601' "$m is a PowerPC 601"
    assert_contains "$m" '"kind":"ppc_601"' "$m has the 601 BAT/segment/HTAB MMU"
    assert_contains "$m" '"fpu":true' "$m FPU capability on"
    assert_contains "$m" '"address_bits":32' "$m is 32-bit"
    assert_contains "$m" '{"unit":3,"label":"ID 3 · CD-ROM bay"}' "$m has the Curio-bus CD-ROM bay"
    assert_contains "$m" '"storage":[{"bus":"scsi","unit":0,"type":"hd"},{"bus":"scsi","unit":3,"type":"cd"}]' "$m opens with a hard disk at ID 0 and the CD-ROM drive at ID 3"
    assert_contains "$m" '"floppies":[{"id":"fd0","label":"Internal floppy drive","types":[{"id":"hd","label":"SuperDrive (1.4 MB)"}],"default":"hd"}]' "$m offers the one internal SuperDrive"
    assert_contains "$m" '{"id":"scsi","label":"SCSI","kind":"scsi"' "$m has the one Curio SCSI bus"
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
    # $C/$D/$E, the numbering the SOFTWARE uses.  An earlier revision
    # declared $B/$C/$D from the schematic silkscreen; that is a board
    # label, not a slot ID.  The pseudo-VIA2 slot bit is `slot - 9`, so a
    # card in $B lands on bit 2 — which nothing enables and nothing
    # services, leaving its /NMRQ latched forever and its slot VBL tasks
    # (the cursor task, when the card is the main screen) never run.  A
    # booted 8100 enables slot-interrupt bits $38 = bits 3/4/5 = $C/$D/$E,
    # always those three, whichever connector holds a card.  See
    # docs/internals/machines/pdm/bart.md and pm8100.c.
    # Apple's names are the silkscreen's, B/C/D, and the detail the slot ID.
    assert_contains "$m" '{"id":"nubus_c","label":"NuBus slot B","detail":"Slot ID $C"' "$m declares socket \$C"
    assert_contains "$m" '{"id":"nubus_d","label":"NuBus slot C","detail":"Slot ID $D"' "$m declares socket \$D"
    assert_contains "$m" '{"id":"nubus_e","label":"NuBus slot D","detail":"Slot ID $E"' "$m declares socket \$E"
    assert_absent "$m" '"id":"nubus_b"' "$m must not offer \$B — nothing services its interrupt bit"
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

# The TNT family: the 7500 keeps the 601, the 8500/9500 are the first
# 604 machines.  The internal MESH bus is wired, so the two HD slots
# are offered, and the SWIM3 + DBDMA-channel-1 floppy datapath is
# complete too -- suite-ans's ans500-diag-floppy boots the Network Server Diagnostic
# Utility from drive 0 -- so the one internal SuperDrive is offered on
# every board in the family.  No NuBus on a PCI machine; PCI slot
# capability arrives with the pluggable-card follow-up.
assert_contains pm7500 '"model":601' "pm7500 is a PowerPC 601"
assert_contains pm7500 '"kind":"ppc_601"' "pm7500 has the 601 MMU"
for m in pm8500 pm9500; do
    assert_contains "$m" '"model":604' "$m is a PowerPC 604"
    assert_contains "$m" '"kind":"ppc_604"' "$m has the 604 split-BAT MMU"
done
for m in pm7500 pm8500 pm9500; do
    assert_contains "$m" '"fpu":true' "$m has the FPU datapath"
    assert_contains "$m" '"address_bits":32' "$m is 32-bit"
    assert_contains "$m" '"nubus":false' "$m has no NuBus"
    assert_contains "$m" '"floppies":[{"id":"fd0","label":"Internal floppy drive","types":[{"id":"hd","label":"SuperDrive (1.4 MB)"}],"default":"hd"}]' "$m offers the one internal SuperDrive"
    assert_contains "$m" '{"id":"scsi","label":"Internal fast SCSI","kind":"scsi"' "$m has the internal MESH bus"
    assert_contains "$m" '{"unit":1,"label":"ID 1 · 3.5″ bay"}' "$m has the 3.5-inch bay at ID 1"
    # No CD-ROM drive until the 53C94 has a bus (pm7500.c).
    assert_contains "$m" '"storage":[{"bus":"scsi","unit":0,"type":"hd"}]' "$m opens with the hard disk alone"
done
assert_contains pm7500 '"freq":100000000' "pm7500 runs at 100 MHz"
assert_contains pm8500 '"freq":120000000' "pm8500 runs at 120 MHz"
assert_contains pm9500 '"freq":132000000' "pm9500 runs at 132 MHz"

# The Apple Network Servers — the same TNT substrate with the Macintosh
# removed.  Both are plain 604s and both advertise PCI; what distinguishes
# them in the profile is the CPU clock, the shipping memory size and the
# 512 MB ROM decode ceiling that replaces the 9500's 1.5 GB (being LESS
# permissive than the silicon is the faithful choice, because above it the
# guest hangs during the RAM test rather than reporting an error).
for m in ans500 ans700; do
    assert_contains "$m" '"model":604' "$m is a PowerPC 604"
    assert_contains "$m" '"kind":"ppc_604"' "$m has the 604 split-BAT MMU"
    assert_contains "$m" '"fpu":true' "$m has the FPU datapath"
    assert_contains "$m" '"address_bits":32' "$m is 32-bit"
    assert_contains "$m" '"nubus":false' "$m has no NuBus"
    assert_contains "$m" '"pci":true' "$m advertises PCI"
    assert_contains "$m" '"video_in":false' "$m has no video digitizer"
    # The bay the diagnostic floppy goes in (suite-ans's ans500-diag-floppy).
    assert_contains "$m" '"floppies":[{"id":"fd0","label":"Internal floppy drive","types":[{"id":"hd","label":"SuperDrive (1.4 MB)"}],"default":"hd"}]' "$m offers the one internal SuperDrive"
    assert_contains "$m" '{"id":"524288","label":"512 MB"}],"default":"65536"' "$m caps RAM at the ROM's 512 MB decode limit and opens at 64 MB"
    # Seven hot-swap bays split across two fast/wide controllers; front bay 0
    # is Apple's expected CD-ROM position, front bay 2 Open Firmware's
    # default boot disk (disk2:aix).  Both fast/wide channels are declared:
    # the second controller is live (tnt_fwscsi_attach binds channel 1 to
    # machine.scsi2, and ans-scsi drives a CD-ROM on it).
    assert_contains "$m" '{"id":"scsi","label":"Internal SCSI bus 0","detail":"Fast and wide SCSI-2 · Symbios 53C825A","kind":"scsi","width":"wide","units":[{"unit":0,"label":"ID 0 · Front bay 0"},{"unit":1,"label":"ID 1 · Front bay 1"},{"unit":2,"label":"ID 2 · Front bay 2"},{"unit":3,"label":"ID 3 · Front bay 3"}]' "$m offers the front bays 0-3 on bus 0"
    assert_contains "$m" '{"id":"scsi2","label":"Internal SCSI bus 1"' "$m offers the second fast/wide bus"
    assert_contains "$m" '"storage":[{"bus":"scsi","unit":0,"type":"cd"},{"bus":"scsi","unit":2,"type":"hd"}],"startup":{"bus":"scsi","unit":2}' "$m opens with the CD in front bay 0 and starts up from front bay 2"
    assert_contains "$m" '{"id":"keyswitch","label":"Keyswitch","kind":"choice","values":[{"id":"unlocked","label":"Unlocked"},{"id":"service","label":"Service"},{"id":"locked","label":"Locked"}],"default":"unlocked"}' "$m's keyswitch is Unlocked by default (D18)"
done
# The 700's two REAR bays cable to fast/wide 1 -- the topology its comment
# described long before the table expressed it.
assert_contains ans700 '{"unit":0,"label":"ID 0 · Rear bracket, top"}' "ans700 declares its upper rear bracket"
assert_contains ans700 '{"unit":1,"label":"ID 1 · Rear bracket, bottom"}' "ans700 declares its lower rear bracket"
assert_not_contains ans500 'Rear bracket' "ans500 has no rear brackets"
assert_contains ans500 '{"unit":0,"label":"ID 0"}' "ans500's bus 1 offers ID 0 (the Windows NT boot disk)"
assert_contains ans700 '{"id":"power_supplies","label":"Power supplies","kind":"choice","values":[{"id":"one","label":"One"},{"id":"two","label":"Two"}],"default":"one"}' "ans700 takes a second supply, shipping with one"
assert_absent ans500 '"id":"power_supplies"' "ans500 has one supply, no option"
assert_contains ans500 '"freq":132000000' "ans500 runs a 132 MHz 604 card"
assert_contains ans700 '"freq":150000000' "ans700 runs a 150 MHz 604 card"
assert_contains ans500 '"memory":"65536"' "ans500 opens at 64 MB (D20)"
assert_contains ans700 '"memory":"65536"' "ans700 opens at 64 MB (D20)"

# The beige Power Macintosh G3 (Gossamer): a 750 behind Grackle, Heathrow's
# MESH bus with two HD slots and the CD at id 3, the SWIM3 SuperDrive, three
# PCI slots and the on-board Rage Pro fixed in slot F1.  The SPD-sized RAM
# decode reaches 768 MB (three 256 MB DIMMs).
for m in pmg3dt pmg3mt; do
    assert_contains "$m" '"model":750' "$m is a PowerPC 750"
    assert_contains "$m" '"kind":"ppc_604"' "$m has the 604-style BAT MMU"
    assert_contains "$m" '"fpu":true' "$m has the FPU datapath"
    assert_contains "$m" '"address_bits":32' "$m is 32-bit"
    assert_contains "$m" '"nubus":false' "$m has no NuBus"
    assert_contains "$m" '"pci":true' "$m advertises PCI"
    assert_contains "$m" '"floppies":[{"id":"fd0","label":"Internal floppy drive","types":[{"id":"hd","label":"SuperDrive (1.4 MB)"}],"default":"hd"}]' "$m offers the one internal SuperDrive"
    assert_contains "$m" '{"id":"scsi","label":"SCSI","detail":"MESH","kind":"scsi"' "$m has the MESH bus"
    assert_contains "$m" '{"id":"ata0","label":"Primary ATA bus","kind":"ata"' "$m declares the primary ATA bus"
    assert_contains "$m" '{"id":"ata1","label":"Secondary ATA bus","kind":"ata"' "$m declares the secondary ATA bus"
    assert_contains "$m" '"storage":[{"bus":"scsi","unit":0,"type":"hd"},{"bus":"scsi","unit":3,"type":"cd"}]' "$m puts the hard disk at SCSI 0 and the CD-ROM at SCSI 3"
    assert_contains "$m" '{"id":"786432","label":"768 MB"}' "$m decodes up to 768 MB"
    assert_contains "$m" '"builtin":{"id":"builtin","label":"Built-in video","detail":"ATI Rage Pro"' "$m's on-board Rage Pro is its built-in video"
    assert_absent "$m" '"id":"ati_rage_pro"' "$m does not offer its on-board chip as a card"
done
assert_contains pmg3dt '"freq":267280000' "pmg3dt runs a 267 MHz 750"
assert_contains pmg3mt '"freq":300690000' "pmg3mt runs a 300 MHz 750"

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

# The three MCU Quadras.  The profile states their CPU, clocks, sockets and
# media; it does not carry an IOP flag or a DAFB monitor list, so those are
# not asserted here.  No RAM ceiling is pinned: the 900's 64 MB and the 950's
# 256 MB on the same board are unsettled (#184).
for m in q700 q900 q950; do
    assert_contains "$m" '"cpu":{"model":68040,"address_bits":32,"fpu":true}' "$m has a 68040 with FPU"
    assert_contains "$m" '"mmu":{"present":true,"kind":"68040"}' "$m has the 68040 MMU"
    assert_contains "$m" '"nubus":true,"pci":false' "$m is a NuBus machine"
    assert_contains "$m" '"video_in":false,"audio_in":false,"aux_cpus":[]' "$m has no video/audio input or aux CPU"
    assert_contains "$m" '"floppies":[{"id":"fd0","label":"Internal floppy drive","types":[{"id":"hd","label":"SuperDrive (1.4 MB)"}],"default":"hd"}]' "$m has one SuperDrive"
    assert_contains "$m" '"storage":[{"bus":"scsi","unit":0,"type":"hd"},{"bus":"scsi","unit":3,"type":"cd"}]' "$m opens with a hard disk at SCSI 0 and its CD at SCSI 3"
    assert_contains "$m" '"builtin":{"id":"builtin","label":"Built-in video","detail":"DAFB"' "$m's DAFB is its built-in video"
done
# The 700 has one 53C96; the 900/950 towers have two -- the internal cable
# and the external chain (machine.scsi2) -- in one SCSI ID space (#185).
assert_contains q700 '"storage":[{"id":"scsi","label":"SCSI","kind":"scsi"' "q700 offers one SCSI bus"
assert_absent q700 '"id":"scsi2"' "q700 has no second bus"
for m in q900 q950; do
    assert_contains "$m" '{"id":"scsi","label":"Internal SCSI","kind":"scsi","width":"narrow"' "$m offers the internal SCSI bus"
    assert_contains "$m" '"shares_units_with":["scsi2"]' "$m's internal bus shares IDs with the external"
    assert_contains "$m" '{"id":"scsi2","label":"External SCSI","kind":"scsi"' "$m offers the external SCSI bus"
    assert_contains "$m" '{"unit":3,"label":"ID 3 · Lower front bay"}' "$m's CD-ROM bay is the lower front bay"
done
assert_contains q700 '"default":"20480"' "q700 opens at 20 MB (D20)"
assert_contains q700 '"freq":25000000' "q700 runs at 25 MHz"
assert_contains q900 '"freq":25000000' "q900 runs at 25 MHz"
assert_contains q950 '"freq":33333333' "q950 runs at 33 MHz"
# NuBus sockets: two on the 700 ($D, $E), five on the 900/950 tower ($A-$E).
assert_contains q700 '"slots":[{"id":"nubus_d","label":"NuBus slot D"' "q700's first socket is D"
assert_contains q700 '{"id":"nubus_e","label":"NuBus slot E"' "q700 has socket E"
assert_absent q700 '"id":"nubus_a"' "q700 has no socket A"
for m in q900 q950; do
    for s in a b c d e; do
        S=$(tr a-z A-Z <<<"$s")
        assert_contains "$m" "{\"id\":\"nubus_$s\",\"label\":\"NuBus slot $S\"" "$m has socket $S"
    done
done

# Every Macintosh has the AppleTalk option, default Active (D22); the Lisa
# and the Macintosh XL do not.
for m in plus se30 iicx iix iifx iici iisi q700 q900 q950 q840av q660av pm6100 pm7100 pm8100 pm7500 pm8500 pm9500 ans500 ans700 pmg3dt pmg3mt; do
    assert_contains "$m" '{"id":"appletalk","label":"AppleTalk","kind":"choice","values":[{"id":"active","label":"Active"},{"id":"inactive","label":"Inactive"}],"default":"active"}' "$m has the AppleTalk option"
done
for m in lisa macxl; do
    assert_absent "$m" '"id":"appletalk"' "$m has no AppleTalk"
    assert_contains "$m" '{"id":"profile","label":"ProFile port","kind":"profile"' "$m's disk is on the ProFile port"
done
assert_contains lisa '{"unit":0,"label":"External"}' "the Lisa 2's ProFile is external"
assert_contains macxl '{"unit":0,"label":"Internal hard disk bay"}' "the Macintosh XL's disk is inside"

# The second floppy position: an external port on the IIcx/IIci/IIsi, a
# second internal bay on the IIx/IIfx, each shipping empty.
for m in iicx iici iisi; do
    assert_contains "$m" '{"id":"fd1","label":"External floppy drive","types":[{"id":"none","label":"None"},{"id":"hd","label":"SuperDrive (1.4 MB)"}],"default":"none"}' "$m's external floppy port ships empty"
done
for m in iix iifx; do
    assert_contains "$m" '{"id":"fd1","label":"Second internal floppy drive","types":[{"id":"none","label":"None"},{"id":"hd","label":"SuperDrive (1.4 MB)"}],"default":"none"}' "$m's second internal bay ships empty"
done

# The floor: every registered model carries at least one assertion above.
models=$(grep -o '^{"id":"[a-z0-9]*"' "$OUT" | sed 's/^{"id":"\(.*\)"/\1/')
[ -n "$models" ] || { echo "FAIL: catalog.models dumped no profiles"; fail=1; }
for m in $models; do
    if [ "${count[$m]:-0}" -eq 0 ]; then
        echo "FAIL: $m: registered, but no capability assertion names it"
        fail=1
    fi
done

if [ "$fail" -ne 0 ]; then
    echo "--- captured profile output ---"
    cat "$OUT"
    exit 1
fi

echo "machine-capabilities: all capability assertions passed"
