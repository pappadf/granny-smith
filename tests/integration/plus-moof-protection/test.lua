-- Plus + a copy-protected MOOF capture (see config.mk).  run.sh makes the
-- capture and boots the Plus from it in drive 0.

local mac = dofile("../lib/mac.lua")

-- What make-protected-moof.py put where (keep in step with it).
local SIG_TRACK, SIG_SECTOR = 40, 2
local MARK_TRACK, MARK_SECTOR = 52, 1

-- A sector's block on an 800K disk: tracks shrink by a sector every 16, and
-- each track holds both sides.
local function block_of(track, side, sector)
  local b = 0
  for t = 0, track - 1 do b = b + 2 * (12 - t // 16) end
  return b + side * (12 - track // 16) + sector
end

-- Occurrences of the byte string `pat` in `s`.
local function count(s, pat)
  local n, at = 0, 1
  while true do
    local i = s:find(pat, at, true)
    if not i then return n end
    n, at = n + 1, i + 1
  end
end

local SIG = "\xDE\xD5\xAA\xAD"
local MARK = "\xB5\x96\xFA\xB5\xDE\xAA" .. string.rep("\xFF", 32) .. "\xD5\xAA\x96\xB3"

local drive = machine.floppy.drive[0]
assert(drive.image.capture, "the MOOF's capture is kept")
assert(drive.image.encoding == "moof+dc42", "a MOOF decodes to its sectors")
assert(drive.image.writable, "INFO does not write-protect this one")

-- Both edits are on the tracks the drive serves -- and only there: a track
-- laid down from the sectors could carry neither.
local function check_tracks(when)
  local sig = drive.track_data(SIG_TRACK, 0)
  local mark = drive.track_data(MARK_TRACK, 1)
  assert(count(sig, SIG) == 1, F"${when}: the signature on track ${SIG_TRACK} (${count(sig, SIG)} found)")
  assert(count(mark, MARK) == 1, F"${when}: the field on track ${MARK_TRACK} (${count(mark, MARK)} found)")
  return sig, mark
end
local sig0, mark0 = check_tracks("inserted")

-- The Finder (CurApName, $910): the Plus booted from the capture.
mac.wait_until(function() return machine.memory.peek.l(0x910) == 0x0646696E end, 20000000, 400000000,
  "the Finder never started")
mac.wait_stable(200000000)
echo("booted")

-- === The guest stub: what a protection check does ========================
--
-- stub.s, assembled (bytes 0..$E9; the data area after it starts zeroed).
local STUB = "41fa00e820b8022e43fa004c21c9022e41fa00ee70132248429951c8fffc317c" ..
  "fffb0018317c0001001643fa012421490020217c000002000024317c0001002c" ..
  "217a00ba002ea00243fa00b0328021fa009a022e60fe48e7fffe41fa00944a50" ..
  "667e30bc0001363c300078003a3a008012146afce18c18010c450001660a0c84" ..
  "ded5aaad671060080c84b596fab5671051cbffde604a41fa005a30bc00016040" ..
  "41fa005030bc00017800760112146afce14c180151cbfff641fa003a30847400" ..
  "12146afc0c0100ff660852420c4203e866ee41fa0022308241fa001e11410001" ..
  "4cdf7fff2f3a00044e75"
local STUB_SIZE = 0x350
local OFF = {done = 0x54, mode = 0xEE, ran = 0xF0, found = 0xF2, mid = 0xF4, count = 0xF6, next = 0xF8,
  rdres = 0xFA, posoff = 0xFC}

-- Run the stub once, in `mode`, on `block` of drive 1, from the Finder's own
-- context: at the entry of GetNextEvent (a point where the application may
-- make any call), with every register put back afterwards.
local function run_stub(mode, block)
  -- The last 1 KB of the screen buffer: nothing else lives there, and the
  -- run is over before the Finder draws again.
  local base = (machine.memory.peek.l(0x824) + 21888 - STUB_SIZE) & ~1
  for i = 0, STUB_SIZE - 1 do
    local hex = STUB:sub(2 * i + 1, 2 * i + 2)
    machine.memory.poke.b(base + i, hex ~= "" and tonumber(hex, 16) or 0)
  end
  machine.memory.poke.w(base + OFF.mode, mode)
  machine.memory.poke.l(base + OFF.posoff, block * 512)
  -- GetNextEvent ($A970): the toolbox trap table at $0C00.
  local gne = machine.memory.peek.l(0x0C00 + 0x170 * 4)
  debug.breakpoints.clear()
  debug.breakpoints.add(gne)
  scheduler.run(100000000)
  assert(machine.cpu.pc == gne, F"never reached GetNextEvent (0x${gne:08x}; pc 0x${machine.cpu.pc:08x})")
  debug.breakpoints.clear()
  local saved = {}
  for _, r in ipairs({"d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7", "a0", "a1", "a2", "a3", "a4", "a5", "a6",
    "a7", "sr", "pc"}) do
    saved[r] = machine.cpu[r]
  end
  machine.cpu.pc = base
  debug.breakpoints.add(base + OFF.done)
  scheduler.run(200000000)
  assert(machine.cpu.pc == base + OFF.done, F"the stub did not finish (pc 0x${machine.cpu.pc:08x})")
  debug.breakpoints.clear()
  local r = {}
  for k, o in pairs(OFF) do
    if k ~= "done" and k ~= "posoff" then r[k] = machine.memory.peek.w(base + o) end
  end
  for k, v in pairs(saved) do
    if k ~= "pc" then machine.cpu[k] = v end
  end
  machine.cpu.pc = saved.pc
  return r
end

-- 1. The driver reads a sector of the signed track; the hook, reading on
--    from the head's position, finds $DE in front of a data mark.
local r = run_stub(1, block_of(SIG_TRACK, 0, SIG_SECTOR))
echo(F"signature: ran=${r.ran} found=${r.found} read=${r.rdres}")
assert(r.ran == 1, "the Sony driver called the hook")
assert(r.found == 1, "the hook found DE D5 AA AD")

-- 2. On the other track, the field that is no sector: its marker, DE AA,
--    exactly 32 self-sync bytes (ten-cell bytes framed one each), then D5.
r = run_stub(2, block_of(MARK_TRACK, 1, MARK_SECTOR))
echo(F"field: ran=${r.ran} found=${r.found} mid=${r.mid:04x} count=${r.count} next=${r.next:02x} read=${r.rdres}")
assert(r.found == 1, "the hook found the marker")
assert(r.mid == 0xDEAA, "DE AA after the marker")
assert(r.count == 32, F"exactly 32 sync bytes (${r.count})")
assert(r.next == 0xD5, "then D5")

-- The machine carries on.
scheduler.run(20000000)
assert(debug.mac.globals.read("Ticks") > 0, "the Mac is still running")

-- === The tracks stay the capture's ========================================
--
-- The boot has written the disk (the Finder's desktop file), so some tracks
-- are spliced; the two protected ones were only read and are the capture's.
local function same_tracks(when)
  local sig, mark = check_tracks(when)
  assert(sig == sig0 and mark == mark0, F"${when}: the protected tracks are what they were")
end
same_tracks("after the boot")

-- A consolidated checkpoint and its restore (which clears the delta bit of
-- every block equal to its base), then a quick one: the same tracks.
assert(checkpoint.save(F"${vars.WORK_DIR}/full.checkpoint"), "consolidated checkpoint saved")
assert(checkpoint.load(F"${vars.WORK_DIR}/full.checkpoint"), "consolidated checkpoint loaded")
drive = machine.floppy.drive[0]
same_tracks("after a consolidated restore")
scheduler.run(5000000)
machine.register("00000000000a0c0f", "20261125T000000Z") -- a machine directory for the quick checkpoint
assert(checkpoint.snapshot("moof"), "quick checkpoint saved")
assert(checkpoint.load(""), "quick checkpoint loaded")
drive = machine.floppy.drive[0]
same_tracks("after a quick restore")

-- The same file in the second drive, read-only: its own image and capture,
-- the same tracks; and after an eject and a reinsert, again.
local ext = machine.floppy.drive[1]
ext.insert(vars.MOOF, false)
assert(ext.image.capture and not ext.image.writable, "the second drive holds the capture, read-only")
assert(ext.track_data(SIG_TRACK, 0) == sig0, "the second drive serves the same signed track")
ext.eject()
ext.insert(vars.MOOF, false)
assert(ext.track_data(MARK_TRACK, 1) == mark0, "and again after a reinsert")
ext.eject()

-- Exported while mounted: the unchanged tracks are copied bit for bit, so
-- the export serves the same protected tracks.
local out = F"${vars.WORK_DIR}/exported.moof"
assert(drive.image.export(out), "export to .moof")
ext.insert(out, false)
assert(ext.track_data(SIG_TRACK, 0) == sig0 and ext.track_data(MARK_TRACK, 1) == mark0,
  "the export keeps the protected tracks")

-- A write-protected disk takes no raw writes: drive 1 selected, the IWM in
-- write mode, bytes written to its data register (the IWM's registers at
-- $DFE1FF + n * $200: n odd sets a line, even clears it).
local side = machine.floppy.sel and 1 or 0
local before = ext.track_data(0, side)
local function iwm(n, v) machine.memory.poke.b(0xDFE1FF + n * 0x200, v or 0) end
iwm(9)  -- ENABLE
iwm(11) -- SELECT: drive 1
iwm(13) -- Q6
for _, b in ipairs({0xD5, 0xAA, 0x96, 0xFF, 0xFF}) do iwm(15, b) end -- Q7: write mode, a byte each
iwm(14); iwm(12); iwm(10); iwm(8) -- the lines as they were
assert(ext.track_data(0, side) == before, "a write-protected disk's track took no raw writes")
assert(ext.image.writes == 0, "nor did its image")
ext.eject()

-- The guest may still read its disk.
scheduler.run(20000000)
echo("protection-ok")
quit()
