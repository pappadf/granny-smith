-- Plus + a MOOF with a weak area (see config.mk).  run.sh
-- makes the capture; the Plus boots from the plain one in drive 0 with the
-- weak one in drive 1.

local mac = dofile("../lib/mac.lua")

-- What make-weak-moof.py put where (keep in step with it).
local WEAK_TRACK, WEAK_SIDE, WEAK_SECTOR = 26, 1, 0
local READS, NIBS, THRESH = 15, 693, 61

-- A sector's block on an 800K disk: tracks shrink by a sector every 16, and
-- each track holds both sides.
local function block_of(track, side, sector)
  local b = 0
  for t = 0, track - 1 do b = b + 2 * (12 - t // 16) end
  return b + side * (12 - track // 16) + sector
end

-- The Finder (CurApName, $910).
mac.wait_until(function() return machine.memory.peek.l(0x910) == 0x0646696E end, 20000000, 400000000,
  "the Finder never started")
mac.wait_stable(200000000)
echo("booted")

local ext = machine.floppy.drive[1]
assert(ext.image.capture, "the weak MOOF's capture is kept")
assert(not ext.image.writable, "INFO write-protects it")

-- === The guest stub: reading the weak area fifteen times ===================
--
-- stub.s, assembled (bytes 0..$169; the data area after it is set here).
local STUB = "267a0168200b6616203c00002edba31e2008670000a0264841fa0150208b41fa" ..
  "015620b8022e43fa011821c9022e3e3a013ebe7a013c6c0000603007c0fc02b5" ..
  "43eb0640d3c041fa0132208941fa013030bc000141fa012a70132248429951c8" ..
  "fffc317cfffb0018317c0002001643eb044021490020217c000002000024317c" ..
  "0001002c217a00ec002ea00241fa00f0425061225247609a21fa00dc022e7000" ..
  "0c53003d6c0a0c6b003d00206d0270013740002260fe41eb0240303c007f4298" ..
  "51c8fffc3007c0fc02b545eb0640d5c07c003a3c02af0c1200f666540c2a00af" ..
  "0001664c0c2a0096000466440c2a00f60005663c122a00030c01009667060c01" ..
  "0097662c7001c001e148102a000241eb02404a300000660650f00000524641eb" ..
  "00404a300000660850f00000526b0020528a51cdffa23007d040378600004e75" ..
  "48e7fffe41fa00384a5067144250227a002a363c02b412146afc12c151cbfff8" ..
  "4cdf7fff2f3a00104e75"
local STUB_SIZE = 0x200
local OFF = {done = 0xB4, heap = 0x16A, first = 0x16E, last = 0x170, posoff = 0x172}
local H = {counts = 0, union = 32, pass = 34, bufs = 1600}

-- Run reads [first, last) of the stub from the Finder's own context: at the
-- entry of GetNextEvent (a point where the application may make any call),
-- with every register put back afterwards.  `heap` is the block an earlier
-- run allocated (0: allocate one).  Returns the block's address.
local function run_stub(heap, first, last)
  -- The last half KB of the screen buffer: nothing else lives there, and
  -- the run is over before the Finder draws again.
  local base = (machine.memory.peek.l(0x824) + 21888 - STUB_SIZE) & ~1
  for i = 0, STUB_SIZE - 1 do
    local hex = STUB:sub(2 * i + 1, 2 * i + 2)
    machine.memory.poke.b(base + i, hex ~= "" and tonumber(hex, 16) or 0)
  end
  machine.memory.poke.l(base + OFF.heap, heap)
  machine.memory.poke.w(base + OFF.first, first)
  machine.memory.poke.w(base + OFF.last, last)
  machine.memory.poke.l(base + OFF.posoff, block_of(WEAK_TRACK, WEAK_SIDE, WEAK_SECTOR) * 512)
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
  scheduler.run(1000000000)
  assert(machine.cpu.pc == base + OFF.done, F"the stub did not finish (pc 0x${machine.cpu.pc:08x})")
  debug.breakpoints.clear()
  local block = machine.memory.peek.l(base + OFF.heap)
  assert(block ~= 0, "the stub has its heap block")
  for k, v in pairs(saved) do
    if k ~= "pc" then machine.cpu[k] = v end
  end
  machine.cpu.pc = saved.pc
  return block
end

-- What the stub found: each read's distinct records, the union, the pass
-- flag, and each read's nibbles as copied.
local function results(block)
  local r = {counts = {}, bufs = {}}
  for k = 0, READS - 1 do
    r.counts[k + 1] = machine.memory.peek.w(block + H.counts + 2 * k)
    r.bufs[k + 1] = machine.memory.peek.bytes(block + H.bufs + k * NIBS, NIBS)
  end
  r.union = machine.memory.peek.w(block + H.union)
  r.pass = machine.memory.peek.w(block + H.pass)
  r.line = table.concat(r.counts, " ") .. F" | union ${r.union} | pass ${r.pass}"
  return r
end

-- An FNV-1a digest of every read's nibbles: compared across processes.
local function digest(r)
  local h = 0x811C9DC5
  for _, b in ipairs(r.bufs) do
    for i = 1, #b do h = ((h ~ b:byte(i)) * 0x01000193) & 0xFFFFFFFF end
  end
  return string.format("%08x", h)
end

-- The weak track reads differently on every revolution, so the check passes
-- -- deterministically, in two runs: reads 0-6, a checkpoint, reads 7-14.
assert(checkpoint.save(F"${vars.WORK_DIR}/finder.checkpoint"), "consolidated checkpoint at the Finder")
local block = run_stub(0, 0, 7)
assert(checkpoint.save(F"${vars.WORK_DIR}/mid.checkpoint"), "consolidated checkpoint between reads")
machine.register("00000000000a0c11", "20261126T000000Z") -- a machine directory for the quick checkpoint
assert(checkpoint.snapshot("weak"), "quick checkpoint between reads")
run_stub(block, 7, READS)
local first = results(block)
local last_draw = ext.track_data(WEAK_TRACK, WEAK_SIDE) -- the draw the drive holds now
echo(F"reads: ${first.line} | digest ${digest(first)}")
if vars.DIGEST_ONLY == "1" then quit() end

local distinct = {}
for _, c in ipairs(first.counts) do distinct[c] = true end
local kinds = 0
for _ in pairs(distinct) do kinds = kinds + 1 end
assert(kinds > 1, "the reads differ from one revolution to the next")
assert(first.counts[1] < THRESH, F"the first read sees fewer than ${THRESH} records (${first.counts[1]})")
assert(first.union >= THRESH, F"all reads together see at least ${THRESH} (${first.union})")
assert(first.pass == 1, "the stub's pass rule holds")
echo("check-passed")

-- Each restore reproduces the remaining reads exactly: the same nibbles,
-- and the drive ends on the same draw.
local function same_rest(when)
  local r = results(block)
  for k = 8, READS do
    assert(r.bufs[k] == first.bufs[k], F"${when}: read ${k - 1} is the one before the restore")
  end
  assert(r.line == first.line, F"${when}: ${r.line}")
  assert(machine.floppy.drive[1].track_data(WEAK_TRACK, WEAK_SIDE) == last_draw, F"${when}: the same last draw")
end
assert(checkpoint.load(F"${vars.WORK_DIR}/mid.checkpoint"), "consolidated checkpoint loaded")
run_stub(block, 7, READS)
same_rest("after a consolidated restore")
assert(checkpoint.load(""), "quick checkpoint loaded")
run_stub(block, 7, READS)
same_rest("after a quick restore")

-- The same commands again from the Finder: the same draws.
assert(checkpoint.load(F"${vars.WORK_DIR}/finder.checkpoint"), "the Finder checkpoint loaded")
assert(run_stub(0, 0, 7) == block, "the same heap block")
checkpoint.save(F"${vars.WORK_DIR}/mid2.checkpoint")
run_stub(block, 7, READS)
local again = results(block)
assert(again.line == first.line and digest(again) == digest(first), F"the same reads again: ${again.line}")
assert(machine.floppy.drive[1].track_data(WEAK_TRACK, WEAK_SIDE) == last_draw, "the same last draw")
echo("deterministic")

scheduler.run(20000000)
assert(debug.mac.globals.read("Ticks") > 0, "the Mac is still running")
echo("weak-bits-ok")
quit()
