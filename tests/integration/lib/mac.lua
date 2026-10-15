-- tests/integration/lib/mac.lua — shared integration-suite library
-- (docs/guide/TESTING.md, "Suites and the shared script library").
--
-- Pull in with `local mac = dofile("../lib/mac.lua")` from a suite's
-- test.lua (paths resolve relative to the including file).  The library
-- reads three optional --var definitions, forwarded by the Makefile via
-- TEST_VARS= and visible here as `vars.NAME`:
--
--   ROW=<label>     run only the row whose label matches (suite row filter)
--   ROW_SET=<a,b,…> run only the rows named in this comma list ("*" = all);
--                   a suite split across directories sets it in each
--                   wrapper's test.lua (`vars.ROW_SET = "…"` before the
--                   dofile). ROW= wins over it, so any row of a split
--                   suite still runs alone from any of its directories.
--   REGEN=1         capture goldens instead of matching them
--   KEEP_GOING=1    a failing row records red + continues (nightly mode);
--                   default is fail-fast (PR mode)
--
-- Row protocol: each suite row is a function whose body asserts its facts,
-- run through mac.row:
--
--   mac.row("q700-chime", function() ... end)
--
-- mac.row runs the function under pcall, so a failed assertion is counted,
-- its screen dumped, and the run either aborted (PR mode) or continued
-- (nightly mode).  suite_done() fails the run at the end when any keep-going
-- row went red.

-- One instance per script state: a suite and the machine libraries it
-- pulls in (ans.lua, nt.lua, gossamer.lua) all dofile this file, and the row
-- counters must be one set.
if _G.__gs_mac then return _G.__gs_mac end
local M = {}
_G.__gs_mac = M

-- --- run-mode bindings (suites read but don't write these) ------------------
M.ROW_FILTER = vars.ROW or ""
M.ROW_SET = vars.ROW_SET or ""
M.REGEN_MODE = vars.REGEN or "0"
M.KEEP_GOING_MODE = vars.KEEP_GOING or "0"
M.RESULTS_DIR = vars.TEST_RESULTS_DIR or "."
M.FIXTURE_DIR = vars.FIXTURES or ""

local rows_run = 0
local rows_failed = 0
local failed_labels = ""

-- --- row harness -------------------------------------------------------------

-- True when the row filter names this row; without one, when the row set
-- is empty or "*" or contains this row.
function M.row_on(label)
  if M.ROW_FILTER ~= "" then return M.ROW_FILTER == label end
  if M.ROW_SET == "" or M.ROW_SET == "*" then return true end
  return gs.contains("," .. M.ROW_SET .. ",", "," .. label .. ",")
end

-- Run a row: skipped when the filter excludes it; on failure dump the
-- screen for diagnosis, then abort (fail-fast) or record red and continue
-- (KEEP_GOING=1).  `err` is the row's error with its traceback.
function M.row(label, fn)
  if not M.row_on(label) then return end
  rows_run = rows_run + 1
  local ok, err = xpcall(fn, luadebug.traceback)
  if ok then
    echo("ROW OK: " .. label)
    return
  end
  if err == gs.quit_signal or err == gs.interrupt_signal then error(err, 0) end
  rows_failed = rows_failed + 1
  failed_labels = failed_labels .. " " .. label
  gs.try(function() machine.screen.save(M.RESULTS_DIR .. "/fail-" .. label .. ".png") end, nil)
  echo("ROW FAILED: " .. label)
  echo(tostring(err))
  if M.KEEP_GOING_MODE ~= "1" then error("row " .. label .. " failed (fail-fast)", 0) end
end

-- Milestone close: the row runs and its result is reported, but red
-- does not fail the suite — milestones track known emulator gaps (A/UX
-- bring-up, RAM-config stalls) until they start passing, at which point
-- they promote to enforced rows via review.
function M.milestone(label, fn)
  if not M.row_on(label) then return end
  rows_run = rows_run + 1
  local ok, err = xpcall(fn, luadebug.traceback)
  if ok then
    echo("MILESTONE GREEN: " .. label .. " - passing now; promote to enforced")
    return
  end
  if err == gs.quit_signal or err == gs.interrupt_signal then error(err, 0) end
  gs.try(function() machine.screen.save(M.RESULTS_DIR .. "/milestone-" .. label .. ".png") end, nil)
  echo("MILESTONE RED: " .. label .. " (known gap; not counted as failure)")
  echo(tostring(err))
end

-- Suite epilogue: report totals and fail the run if keep-going rows went
-- red. Call as the suite's last statement before `quit()`.
-- (Spelled with `..`, not F"…": a module local that a function names only
-- inside a template is not one of its upvalues, so F cannot see it.)
function M.suite_done()
  echo("rows: " .. rows_run .. " run, " .. rows_failed .. " failed" .. failed_labels)
  assert(rows_failed == 0, "suite:" .. rows_failed .. " row(s) failed:" .. failed_labels)
end

-- --- goldens -----------------------------------------------------------------

-- Match `ref` against the screen — or capture it when REGEN=1 (adopted
-- from iifx-install-76). Regenerating a suite is
--   make test-<suite> TEST_VARS="REGEN=1"   + a reviewed git diff.
function M.check(ref)
  if M.REGEN_MODE == "1" then
    machine.screen.save(ref)
  else
    machine.screen.match(ref)
  end
end

-- Like check(), but the match excludes the (top,left,bottom,right)
-- rectangle — for goldens carrying nondeterministic chrome the row
-- can't pin (System 7.5+'s menu-bar clock is the canonical case; pin
-- the RTC too so the rest of the frame is stable).
function M.check_ex(ref, top, left, bottom, right)
  if M.REGEN_MODE == "1" then
    machine.screen.save(ref)
  else
    machine.screen.match(ref, top, left, bottom, right)
  end
end

-- Two exclusion rectangles, for a frame carrying two phase-dependent
-- fields with content worth asserting BETWEEN them — so a bounding box
-- over the pair would blank the assertion too.
--
-- (About This Macintosh, the case that forced it, has its own helper:
-- check_about below.)
function M.check_ex2(ref, top, left, bottom, right, top2, left2, bottom2, right2)
  if M.REGEN_MODE == "1" then
    machine.screen.save(ref)
  else
    machine.screen.match(ref, top, left, bottom, right, top2, left2, bottom2, right2)
  end
end

-- Sound goldens: stop the running capture and either match `ref` or, in
-- REGEN mode, write the capture to it. `machine.sound.capture.stop` takes
-- an optional WAV path precisely for this, so sound goldens can be
-- regenerated the same way screen goldens are — which matters because a
-- capture window is host-specific: the same Sound Manager beep spans 5,423
-- frames on a 68030 IIcx and 5,426 on a 68020 IIx, so re-hosting a sound
-- row means recapturing, not reusing.
-- Frames are asserted BEFORE the compare so a window-length change reports
-- as such instead of as an opaque waveform mismatch — but only on the verify
-- path. REGEN echoes the count it captured rather than asserting it, because
-- the whole reason to REGEN a re-hosted row is that the old count no longer
-- holds. Existing chime rows still stop/assert/match by hand; they can adopt
-- this as they are touched.
function M.check_sound(ref, want_frames)
  if M.REGEN_MODE == "1" then
    machine.sound.capture.stop(ref)
    echo(F"REGEN sound ${ref}: captured ${machine.sound.capture.frames} frames (update the check_sound call to match)")
  else
    machine.sound.capture.stop()
    assert(machine.sound.capture.frames == want_frames,
      F"sound ${ref}: capture length ${machine.sound.capture.frames}, want ${want_frames}")
    machine.sound.match(ref)
  end
end

-- About This Macintosh, matched with its guest-memory figures excluded.
-- "Largest Unused Block" (133,440)-(148,490) and the System Software row's
-- figure and bar (152,250)-(166,490) move with how much heap the System
-- claimed on the way up, which any change in boot-time timing or input
-- shifts by a few KB; nothing here can act on that, so the rows don't pin
-- it. "Total Memory" -- what these rows exist to check -- stays asserted.
-- Same window position on every 640-wide desktop. check_about_clock also
-- drops System 7.5's menu-bar clock.
function M.check_about(ref)
  if M.REGEN_MODE == "1" then
    machine.screen.save(ref)
  else
    machine.screen.match(ref, 133, 440, 148, 490, 152, 250, 166, 490)
  end
end

function M.check_about_clock(ref)
  if M.REGEN_MODE == "1" then
    machine.screen.save(ref)
  else
    machine.screen.match(ref, 133, 440, 148, 490, 152, 250, 166, 490, 0, 440, 20, 640)
  end
end

-- --- waits: a wait states its condition; ceilings are hang ------------------
-- detectors, not tuning. All return the instructions actually spent, the
-- per-row perf telemetry.

-- The one loop under every wait below: run `step` instructions at a time
-- until cond() holds, failing with `what` once `ceiling` is spent.  Returns
-- the instructions spent.
function M.wait_until(cond, step, ceiling, what)
  local spent = 0
  while not cond() do
    assert(spent < ceiling, F"${what}: not reached within ${ceiling}")
    scheduler.run(step)
    spent = spent + step
  end
  return spent
end

-- Run until the screen checksum is unchanged for 3 consecutive polls of
-- `step` instructions, up to `ceiling` (the field-proven iifx-install-76
-- settle()).  Returns the instructions spent.
local function wait_still(step, ceiling, what)
  local previous, stable = 0, 0
  return M.wait_until(function()
    local now = machine.screen.checksum()
    if now == previous then stable = stable + 1 else stable = 0 end
    previous = now
    return stable >= 3
  end, step, ceiling, what)
end

-- Run until the screen matches `golden`, in `step`-instruction quanta,
-- up to `ceiling`. On timeout, screen.match fails the row with the usual
-- diff artifacts. REGEN mode can't wait on a golden that doesn't exist
-- yet, so it polls at the SAME granularity until the screen holds still
-- and captures that frame.
--
-- Use this for terminal "reach this screen" rows. A row that goes on to
-- INTERACT (menus, clicks) should use `wait_stable` + `check` instead:
-- wait_match stops at the first quantum whose frame equals the golden,
-- which can be a few quanta before the guest is actually quiescent, and
-- a capture-time/verify-time difference in that slack changes how the
-- interaction lands. wait_stable behaves identically in both modes.
function M.wait_match(golden, ceiling, step)
  if M.REGEN_MODE == "1" then
    local spent = wait_still(step, ceiling, "wait_match(REGEN): screen never settled")
    machine.screen.save(golden)
    return spent
  end
  local spent = 0
  while spent < ceiling do
    scheduler.run(step)
    spent = spent + step
    if machine.screen.matches(golden) then return spent end
  end
  machine.screen.match(golden)
  return spent
end

-- A fixed budget that ends early when the screen is already the golden.
-- For the screens a stability wait cannot find (a floppy boot's long quiet
-- stretches, a benchmark's blank phases, "Formatting disk..." held for the
-- whole format): verify mode polls every `step` and returns at the first
-- frame that matches `golden` (or after `ceiling`, leaving the caller's
-- check() to fail with the usual diff).  REGEN polls the same way while the
-- golden exists, so both modes run the same timeline and the goldens after
-- this one are captured where verify will look for them; only a golden that
-- does not exist yet gets the whole `ceiling`.  The caller still
-- check()s afterwards.  Returns the instructions spent.
function M.run_until_match(golden, ceiling, step)
  if M.REGEN_MODE == "1" and not M.have_media(golden) then
    scheduler.run(ceiling)
    return ceiling
  end
  local spent = 0
  while spent < ceiling do
    scheduler.run(step)
    spent = spent + step
    if machine.screen.matches(golden) then return spent end
  end
  return spent
end

-- A head start and a stability wait that end on `golden`: verify mode polls
-- for the golden every `step`, up to head + ceiling, and the caller's check()
-- fails as usual if it never came.  REGEN does the same while the golden
-- exists (one timeline in both modes); a golden not captured yet gets the
-- `head` budget and a wait for the screen to hold still (up to `ceiling`).
-- Returns the instructions spent.
function M.reach(golden, head, ceiling, step)
  if M.REGEN_MODE == "1" and not M.have_media(golden) then
    scheduler.run(head)
    return head + M.wait_stable(ceiling)
  end
  return M.run_until_match(golden, head + ceiling, step)
end

-- run_until_match, comparing outside the rectangle check_ex() excludes.
function M.run_until_match_ex(golden, top, left, bottom, right, ceiling, step)
  if M.REGEN_MODE == "1" and not M.have_media(golden) then
    scheduler.run(ceiling)
    return ceiling
  end
  local spent = 0
  while spent < ceiling do
    scheduler.run(step)
    spent = spent + step
    if machine.screen.matches(golden, top, left, bottom, right) then return spent end
  end
  return spent
end

-- Run until the screen checksum is unchanged for 3 consecutive polls
-- (the field-proven iifx-install-76 settle()), up to `ceiling`.
function M.wait_stable(ceiling)
  return wait_still(20000000, ceiling, "wait_stable: screen never settled")
end

-- Run until the Finder is the current application (low-memory CurApName,
-- $910, reads the Pascal string "Finder"), then until the screen holds
-- still: the boot-to-desktop wait.  It replaces a fixed head start before
-- wait_stable (a stability wait alone can settle on a quiet moment of the
-- extension parade), so it ends as soon as the desktop is up.  `ceiling`
-- bounds each half.
function M.wait_desktop(ceiling)
  local spent = M.wait_until(function()
    return gs.try(function() return machine.cpu.mmu.peek(0x910, 4) end, 0) == 0x0646696E
      and gs.try(function() return machine.cpu.mmu.peek(0x914, 2) end, 0) == 0x6465
  end, 20000000, ceiling, "wait_desktop: the Finder never became the current application")
  return spent + M.wait_stable(ceiling)
end

-- Run until the screen checksum differs from `before` (menu opened,
-- dialog appeared, cursor moved), up to `ceiling`.
function M.wait_change(before, ceiling)
  return M.wait_until(function() return machine.screen.checksum() ~= before end,
    5000000, ceiling, "wait_change: screen never changed")
end

-- Run until lowmem global `name` reads `want`, up to `ceiling`.
function M.wait_global(name, want, ceiling)
  return M.wait_until(function() return debug.mac.globals.read(name) == want end,
    5000000, ceiling, F"wait_global: ${name} != ${want}")
end

-- --- run until the guest has taken the input --------------------------------
--
-- The input side of the waits above.  A script hands input to the machine
-- at once, but the guest takes it on its own schedule: a serial line
-- delivers one character per character time (scc.c, line_in) and the ADB
-- mouse one report per poll (adb.c, mouse_queue).  These run the machine in
-- small slices until the input is consumed, or until the guest has said
-- what the caller waits for, instead of a budget sized for the slowest
-- machine.  Each slice is a host round trip of about a millisecond on top
-- of the emulation, so the slices start small and double, which bounds the
-- overshoot at the time the wait itself took.

-- Run until the guest has taken every pending host mouse event (motion and
-- button changes, queued ones included), up to `ceiling`.  Returns the
-- instructions spent.
function M.wait_mouse_taken(ceiling)
  return M.wait_until(function() return not machine.adb.mouse.pending end,
    250000, ceiling, "wait_mouse_taken: the guest did not take the mouse input")
end

-- Run until the guest has taken every key keyboard.type queued (scheduled
-- transitions and the keyboard's own queue), up to `ceiling`.  Returns the
-- instructions spent.
function M.wait_keys_taken(ceiling)
  return M.wait_until(function() return not machine.adb.keyboard.pending end,
    250000, ceiling, "wait_keys_taken: the guest did not take the typed keys")
end

-- A budget that ends early once the screen holds still: the guest's reaction
-- to an input (a menu closing, a dialog drawn, a window opened) has finished
-- when the screen has not changed for a third of a second (sampled every 5
-- ticks, 4 equal samples).  Never longer than `ceiling`, the fixed budget it
-- replaces, and never a failure at it: a screen that keeps moving (a
-- blinking caret, an animation) simply runs the old budget.  Not for waits
-- whose end the screen does not show (disk or network work behind a static
-- window) -- those need their own condition.  Returns the instructions spent.
function M.settle(ceiling)
  local spent, still = 0, 0
  local previous = machine.screen.checksum()
  local t = debug.mac.globals.read("Ticks")
  while still < 4 and spent < ceiling do
    scheduler.run(200000)
    spent = spent + 200000
    if debug.mac.globals.read("Ticks") >= t + 5 then
      t = debug.mac.globals.read("Ticks")
      local now = machine.screen.checksum()
      if now == previous then still = still + 1 else still = 0 end
      previous = now
    end
  end
  return spent
end

-- settle() for a guest that keeps no Mac OS Ticks (Open Firmware, a
-- diagnostic): the screen is sampled every `step` instructions and has held
-- still once 4 samples agree.  Bounded at `ceiling`, the fixed budget it
-- replaces, without failing there.  Returns the instructions spent.
function M.settle_steps(step, ceiling)
  local spent, still = 0, 0
  local previous = machine.screen.checksum()
  while still < 4 and spent < ceiling do
    scheduler.run(step)
    spent = spent + step
    local now = machine.screen.checksum()
    if now == previous then still = still + 1 else still = 0 end
    previous = now
  end
  return spent
end

-- Type `text` on the ADB keyboard and run until the guest has taken the keys
-- and the screen has held still (settle_steps), up to `ceiling` -- for a
-- command typed on a console that is the screen.  Returns the instructions
-- spent.
function M.type_settle(text, step, ceiling)
  machine.adb.keyboard.type(text)
  local spent = M.wait_keys_taken(ceiling)
  return spent + M.settle_steps(step, ceiling)
end

-- Drain and return what serial channel `ch` ("a" or "b") has transmitted.
function M.serial_sent(ch)
  if ch == "a" then return machine.scc.a.sent() end
  return machine.scc.b.sent()
end

-- Characters `receive` sent on serial channel `ch` that the guest has not
-- read yet (still on the line, or in the receive FIFO).
function M.serial_rx_pending(ch)
  if ch == "a" then return machine.scc.a.rx_pending end
  return machine.scc.b.rx_pending
end

-- Type `text` on serial channel `ch` as a terminal on the cable would (one
-- character time apart at the channel's programmed rate) and run until the
-- guest has read all of it, up to `ceiling`.  What the guest transmits
-- meanwhile (its echo) stays in the capture for the caller.  Returns the
-- instructions spent.
function M.serial_type(ch, text, ceiling)
  local queued
  if ch == "a" then
    queued = machine.scc.a.receive(text)
  else
    queued = machine.scc.b.receive(text)
  end
  assert(queued == #text, F"serial_type: the line took ${queued} of ${#text} characters")
  return M.wait_until(function() return M.serial_rx_pending(ch) <= 0 end,
    100000, ceiling, "serial_type: the guest did not read the line")
end

-- Run until serial channel `ch` has transmitted `needle`, up to `ceiling`,
-- and return everything it transmitted on the way (the caller asserts on
-- it; a reply that never contains `needle` comes back as it stands).
-- Slices start at `step` and double up to 64 times that.
function M.run_until_sent(ch, needle, step, ceiling)
  local out, spent, slice = "", 0, step
  while not gs.contains(out, needle) and spent < ceiling do
    scheduler.run(slice)
    spent = spent + slice
    out = out .. M.serial_sent(ch)
    if slice < step * 64 then slice = slice * 2 end
  end
  return out
end

-- --- guest-time choreography -------------------------------------------------

-- Run for `n` guest ticks (1/60 s each): interaction delays are guest-
-- time constraints (DoubleTime is in ticks), so choreography built on
-- this ports across CPU speeds unchanged.
--
-- `Ticks` is maintained by the Mac OS interrupt handler, so it stops
-- advancing the moment the guest stops being Mac OS (the MkLinux Booter's
-- hand-off to the Mach kernel, a system error, a hung interrupt path) --
-- and an unbounded wait for it never ends.  The ceiling below is the same
-- rule wait_stable and wait_match already follow: an instruction budget,
-- and a loud failure when it is spent.  A tick costs ~65 k instructions
-- on an SE/30 and ~550 k on a 601/604 (freq / CPI over 60 Hz), so 4 M per
-- tick leaves the slowest-per-tick machine ~7x of headroom -- enough that
-- only a guest that has genuinely stopped keeping Ticks hits it.  The +30
-- floors short waits, which are the ones a stalled tick would trip first.
M.RUN_TICKS_BUDGET = 4000000

function M.run_ticks(n)
  local t0 = debug.mac.globals.read("Ticks")
  local ceiling = (n + 30) * M.RUN_TICKS_BUDGET
  M.wait_until(function() return debug.mac.globals.read("Ticks") >= t0 + n end,
    500000, ceiling,
    F"run_ticks: Ticks did not advance ${n} from ${t0} - has the guest left Mac OS?")
end

-- Run until the guest's cursor task has taken a "global" move: the move
-- sets CrsrNew and the cursor VBL task clears it once it has drawn the
-- cursor at the new spot, within a tick on every machine measured.  Bounded
-- at `ticks` guest ticks, after which it simply goes on: a guest whose
-- cursor task does not use CrsrNew loses nothing against the old fixed wait.
function M.wait_cursor(ticks)
  local t0 = debug.mac.globals.read("Ticks")
  local spent = 0
  while debug.mac.globals.read("CrsrNew") ~= 0 and debug.mac.globals.read("Ticks") < t0 + ticks do
    assert(spent < (ticks + 30) * M.RUN_TICKS_BUDGET,
      "wait_cursor: Ticks did not advance - has the guest left Mac OS?")
    scheduler.run(50000)
    spent = spent + 50000
  end
  return spent
end

-- Move-press-release SINGLE click at screen coordinates. The move is a real
-- move to somewhere the cursor is not already parked: a same-spot warp emits no
-- ADB motion, so under E-clock-synced ADB the click that follows has no fresh
-- position event to bind to and is dropped. Callers that must click the same
-- button twice therefore step away in between rather than repeating click().
--
-- The press and release go in back to back: the mouse model holds each
-- button level until the guest has seen it (ADB: one report per transition;
-- quadrature: at least one VBL), so the click arrives whole, and the row
-- runs on as soon as the guest has taken it rather than after a fixed
-- 30 ticks.
function M.click(x, y)
  machine.adb.mouse.move(x, y, "global")
  M.wait_cursor(20)
  machine.adb.mouse.click(true)
  machine.adb.mouse.click(false)
  M.wait_mouse_taken(100000000)
  M.run_ticks(2)
end

-- Press-release-press-release DOUBLE click at screen coordinates, well inside
-- DoubleTime on every machine.  The two clicks are two ticks apart: with the
-- second press in the same tick as the first release, the Finder (7.5 on the
-- IIci, measured) takes them as two single clicks.
function M.double_click(x, y)
  machine.adb.mouse.move(x, y, "global")
  M.wait_cursor(20)
  machine.adb.mouse.click(true)
  machine.adb.mouse.click(false)
  M.wait_mouse_taken(100000000)
  M.run_ticks(2)
  machine.adb.mouse.click(true)
  machine.adb.mouse.click(false)
  M.wait_mouse_taken(100000000)
end

-- The fixed-tick click and double click click() and double_click() used to
-- be (20 ticks after the move, 4 held, 6 after), for rows whose capture is
-- pinned to the boot's exact timeline -- the speech-recognition rows, where
-- which take the recognizer lands moves with any change to it.
function M.click_fixed(x, y)
  machine.adb.mouse.move(x, y, "global")
  M.run_ticks(20)
  machine.adb.mouse.click(true)
  M.run_ticks(4)
  machine.adb.mouse.click(false)
  M.run_ticks(6)
end

function M.double_click_fixed(x, y)
  machine.adb.mouse.move(x, y, "global")
  M.run_ticks(20)
  machine.adb.mouse.click(true)
  M.run_ticks(4)
  machine.adb.mouse.click(false)
  M.run_ticks(6)
  machine.adb.mouse.click(true)
  M.run_ticks(4)
  machine.adb.mouse.click(false)
end

-- Apple menu → first item ("About ...") via a fixed guest-tick render
-- window; the caller matches or checks the resulting frame. Fixed, not
-- a stability wait, deliberately: the About box's memory figures
-- update live when another heap is active (e.g. a control panel DA
-- open), so the screen may never settle — determinism makes the fixed
-- instant phase-exact instead. Byte-identical dance previously pasted
-- into the three Quadra HD tests.
function M.about_box()
  -- The first warp after a boot is swallowed, so step on a neutral spot
  -- before aiming at the menu title. Tick counts are deliberately
  -- generous (~30 ticks per step): the Menu Manager and the ADB poll
  -- need tens of ticks to track a warp, and the threshold varies per
  -- machine — measured on the IIci, 8 ticks (~0.8 M instructions there)
  -- opens nothing while 30 works everywhere tried.
  machine.adb.mouse.move(50, 50, "global")
  M.run_ticks(30)
  machine.adb.mouse.move(25, 10, "global")
  M.run_ticks(20)
  machine.adb.mouse.click(true)
  M.run_ticks(40)
  machine.adb.mouse.move(25, 30, "global")
  M.run_ticks(40)
  machine.adb.mouse.click(false)
  M.run_ticks(150)
end

-- Same choreography for machines with a QUADRATURE mouse (Plus, and any
-- pre-ADB model): the moves must NOT use "global". Measured 2026-07-28 —
-- with a global warp the Menu Manager highlights the item but the release
-- never commits the selection, so the About window never opens; plain
-- positional moves work. Rows on ADB machines use about_box() above.
function M.about_box_quad()
  machine.adb.mouse.move(14, 8)
  M.run_ticks(20)
  machine.adb.mouse.click(true)
  M.run_ticks(30)
  machine.adb.mouse.move(25, 30)
  M.run_ticks(30)
  machine.adb.mouse.click(false)
  M.run_ticks(150)
end

-- --- boot helpers ------------------------------------------------------------
--
-- SUITE-AUTHOR NOTE on machine.boot inside a suite: the document is the
-- whole specification -- an argument a row leaves out is the model's own
-- default, never what the PREVIOUS row booted.  A row whose behaviour
-- depends on the video card, the vROM or the monitor sense names them.
-- (Found the hard way: suite-iix's portrait row inherited the 8•24 GC
-- from the card-seat row two rows earlier and never applied the portrait
-- sense, while passing in isolation.)

-- (The Universal ROM's ~13 s boot-drive-discovery wait needs no bypass: a
-- machine is built with PRAM whose Start Manager byte already disables it —
-- rtc.h pram_defaults_t.  skip_boot_wait(), a KeyMap Cmd-Shift-Power poke,
-- is gone with it.  A row that wants the wait clears $01 bit 7.)

-- Assert the booted System's addressing mode: MMU32bit ($0CB2)
-- is 0 in 24-bit mode, nonzero in 32-bit mode.
--
-- Only meaningful on 68020-and-later machines. The 68000 models (Plus,
-- Lisa/XL) are always 24-bit and never define this global — $0CB2 there
-- holds unrelated lowmem (measured: it reads $70 on a Plus under 6.0.8),
-- so those rows must not call this.
function M.assert_addr(want32)
  local m = debug.mac.globals.read("MMU32bit")
  -- A value outside {0,1} means the guest never wrote the global — in
  -- practice the machine did not finish booting, and the byte is
  -- uninitialised memory. (Measured 2026-07-28: a IIci at 5 MB reads
  -- $6D here, which is a byte of the $DB6DB6DB uninitialised pattern
  -- MemTop shows too; the machine never completes POST below 8 MB.)
  -- Fail with the raw byte rather than silently reading garbage as an
  -- addressing mode.
  assert(m == 0 or m == 1, F"addressing: MMU32bit=${m:02x} is not a boolean in this configuration")
  if want32 then
    assert(m == 1, F"addressing: expected 32-bit, MMU32bit=${m:02x}")
  else
    assert(m == 0, F"addressing: expected 24-bit, MMU32bit=${m:02x}")
  end
end

-- True when optional media exists — the media-gated skip pattern: rows
-- whose prepared image has not been fetched echo "skip:" and report an
-- uncovered target cell instead of failing.
function M.have_media(path)
  return files.path_exists(path)
end

-- --- checkpoint fixtures (docs/guide/TESTING.md, "Checkpoint fixtures") ------
-- A producer test (config.mk: TEST_PROVIDES := name) saves the machine at a
-- point other tests start from; a consumer (TEST_NEEDS := name) loads it
-- instead of booting there itself.  Only `make test` provides a fixture
-- directory; a test run on its own gets none, so a consumer always has its
-- own way to the same state:
--
--   if not mac.fixture_load("ans500-of-prompt") then boot_to_prompt("ans500") end
--
-- The fixture must be the state that way reaches, byte for byte (the boot is
-- deterministic), or the consumer's assertions would depend on how it ran.

-- Save the machine as fixture `name`, when the run has a fixture directory.
-- Written under a temporary name and renamed, so no reader sees part of it.
function M.fixture_save(name)
  if M.FIXTURE_DIR == "" then return false end
  local f = M.FIXTURE_DIR .. "/" .. name .. ".gsc"
  assert(checkpoint.save(f .. ".part"), F"fixture ${name}: checkpoint.save failed")
  files.mv(f .. ".part", f)
  echo("fixture: saved " .. name)
  return true
end

-- Load fixture `name`; false (and nothing changed) when the run has none.
function M.fixture_load(name)
  if M.FIXTURE_DIR == "" then return false end
  local f = M.FIXTURE_DIR .. "/" .. name .. ".gsc"
  if not files.path_exists(f) then
    echo(F"fixture: ${name} not available (its producer failed or did not run), booting instead")
    return false
  end
  assert(checkpoint.load(f), F"fixture ${name}: checkpoint.load failed")
  echo("fixture: loaded " .. name)
  return true
end

-- The whole test could not run (its media is not in the fetched test data):
-- the "skip:" line the coverage contract reads, and the @@SKIP record the
-- runner turns into `=== SKIP: <test> (<why>) ===` instead of PASS.  `why`
-- starts with the test's name, as the skip: lines always have.
function M.skip_test(why)
  echo("skip: " .. why)
  echo("@@SKIP " .. why)
end

-- --- performance telemetry ---------------------------------------------------

-- Emit this row's deterministic instruction spend; nightly tooling
-- diffs these against perf-baselines.json.
function M.report_perf(row, instr)
  echo('@@PERF {"row":"' .. row .. '","instr":' .. tostring(instr) .. '}')
end

-- Host-throughput floor (coarse, 3-5x below healthy): t0 is a
-- scheduler.host_user_ns snapshot from before the row's runs. Disabled
-- by --var PERF_FLOORS=off (the Valgrind runner).
function M.mips_floor(label, t0, floor_mips)
  local mips = scheduler.instr_count * 1000 // (scheduler.host_user_ns - t0)
  echo(F"${label}: ${mips} MIPS")
  if (vars.PERF_FLOORS or "on") ~= "off" then
    assert(scheduler.instr_count * 1000000000 // (scheduler.host_user_ns - t0) > floor_mips * 1000000,
      F"${label}: emulator throughput below ${floor_mips} MIPS")
  end
end

-- --- coverage records --------------------------------------------------------

-- Emit the machine-read coverage record for a row that reached its
-- golden: machine, RAM, geometry, depth and addressing mode are read from
-- the LIVE machine, so the record cannot drift from what actually ran;
-- `system` and `card` are declared by the row (reading the booted
-- System's `vers` resource from the guest instead of trusting the row is
-- a follow-up — three media labels in this tree turned out to lie).
--
-- addr32 tests for exactly 1, not "nonzero": on a 68000 there is no
-- MMU32bit global at all and $0CB2 holds unrelated lowmem (a Plus reads
-- $70), so a nonzero test would report those rows as 32-bit. Every
-- machine measured that really is in 32-bit mode writes exactly 1.
--
-- scripts/test-matrix.py --check diffs these against the declared roster
-- in matrix-targets.json.
local function cov_record(system, card, feature, addr32)
  echo('@@COV {"machine":"' .. machine.id .. '","ram":' .. tostring(machine.ram)
    .. ',"system":"' .. system .. '","card":"' .. card
    .. '","width":' .. tostring(machine.screen.width)
    .. ',"height":' .. tostring(machine.screen.height)
    .. ',"depth":' .. tostring(machine.screen.depth)
    .. ',"addr32":' .. addr32 .. ',"feature":"' .. feature .. '"}')
end

function M.report_row(system, card, feature)
  cov_record(system, card, feature, tostring(debug.mac.globals.read("MMU32bit") == 1))
end

-- The same record for a row running an OS other than Mac OS (MkLinux, AIX,
-- Open Firmware): addr32 is the Mac OS global MMU32bit, which such an OS does
-- not have, so it is reported as null rather than as a false reading.
function M.report_row_foreign(system, card, feature)
  cov_record(system, card, feature, "null")
end

return M
