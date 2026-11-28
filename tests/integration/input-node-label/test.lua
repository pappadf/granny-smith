-- The keyboard-and-mouse node's label and doc per machine (see config.mk).

local function member_of(ms, name)
    for _, m in ipairs(ms) do
        if m.name == name then return m end
    end
    return nil
end

-- Every label the machine node's children show: what the SYSTEM tab lists.
local function shown_labels()
    local seen = {}
    for _, m in ipairs(machine.meta.members()) do
        if m.kind == "child" then seen[m.label] = true end
    end
    return seen
end

-- The active machine's input node shows `label`, its doc mentions `doc_word`,
-- and "ADB" is in the device list exactly when `has_adb`.  The path and the
-- two devices under it are the same either way.
local function expect_input_node(what, label, doc_word, has_adb)
    assert(machine.adb.meta.path == "machine.adb", F"${what}: the path is machine.adb")
    assert(machine.adb.meta.label == label, F"${what}: label '${machine.adb.meta.label}', want '${label}'")
    local row = member_of(machine.meta.members(), "adb")
    assert(row and row.kind == "child", F"${what}: machine lists the node")
    assert(row.label == label, F"${what}: the tree row reads '${row.label}', want '${label}'")
    assert(row.doc:find(doc_word, 1, true), F"${what}: the row's doc '${row.doc}' does not mention ${doc_word}")
    assert(machine.adb.meta.doc == row.doc, F"${what}: the node's doc is the row's")
    local shown = shown_labels()
    local verdict = has_adb and "missing" or "listed"
    assert((shown["ADB"] == true) == has_adb, F"${what}: an ADB row is ${verdict}")
    assert((row.doc:find("Apple Desktop Bus", 1, true) ~= nil) == has_adb, F"${what}: doc '${row.doc}'")
    -- The devices under it, by the same names and labels on every machine.
    local kids = machine.adb.meta.members()
    assert(member_of(kids, "keyboard").label == "Keyboard", F"${what}: keyboard row")
    assert(member_of(kids, "mouse").label == "Mouse", F"${what}: mouse row")
    -- And the old spelling still reaches them.
    machine.adb.keyboard.press("a")
    machine.adb.mouse.move(10, 10)
    scheduler.run(200000)
end

local function boot(model, rom)
    machine.boot{model = model, rom = F"../../data/roms/${rom}"}
    assert(machine.id == model, F"${model}: not built")
    scheduler.run(200000)
end

-- No ADB: the Plus's keyboard hangs off the VIA and its mouse off the SCC/VIA.
assert(machine.id == "plus", "the harness booted a Plus")
expect_input_node("plus", "Keyboard & Mouse", "Keyboard and mouse", false)

-- No ADB: the Lisa and the Macintosh XL (the same board) have a COPS.
boot("lisa", "lisa2-revh-098917b2.rom")
expect_input_node("lisa", "COPS", "COPS", false)
boot("macxl", "macxl-3a-094c82f0.rom")
expect_input_node("macxl", "COPS", "COPS", false)

-- ADB Macs: a 68K one behind the VIA, a Power Mac behind Cuda.
boot("se30", "iix-iicx-se30-97221136.rom")
expect_input_node("se30", "ADB", "Apple Desktop Bus", true)
boot("pm6100", "pm6100-pm7100-pm8100-9feb69b3.rom")
expect_input_node("pm6100", "ADB", "Apple Desktop Bus", true)

-- The label follows the machine back down again...
boot("lisa", "lisa2-revh-098917b2.rom")
expect_input_node("lisa after an ADB Mac", "COPS", "COPS", false)
boot("plus", "plus-v3-4d1f8172.rom")
expect_input_node("plus after a Lisa", "Keyboard & Mouse", "Keyboard and mouse", false)

-- ...and with no machine the node names no machine's wire.
boot("se30", "iix-iicx-se30-97221136.rom")
assert(machine.shutdown(), "machine.shutdown")
local idle = member_of(machine.meta.members(), "adb")
assert(idle == nil or idle.label == "Keyboard & Mouse", F"no machine: the row reads '${idle and idle.label}'")

-- (A machine again: a script that quits with none reports failure.)
boot("plus", "plus-v3-4d1f8172.rom")

quit()
