# Lisa COPS microcontroller — implementation notes

`src/core/peripherals/cops.{c,h}` models the Apple Lisa **COPS** (National
COP421-class) microcontroller, which services the keyboard, mouse, real-time
clock, and soft-power through the A-port of VIA1. [docs/lisa.md](lisa.md) §11 is
the hardware reference; this note records the implementation and the host-side
protocol verified against the rev-H boot ROM (`Lisa Boot ROM RM248.{K,S,M}`).

## How it attaches

The COPS is not memory-mapped; it lives behind **VIA1**. `lisa.c` routes VIA1's
output callback to `cops_via_output()` and the COPS drives VIA1 input pins
(`via_input`, `via_input_c`) — so the model acts only on real VIA pin traffic,
never on guest memory.

## Protocol (host ↔ COPS)

### Command path — `COPSCMD`
1. Host writes the command byte to VIA1 port A (`PORTA1`, reg 15, no handshake).
2. Host polls **VIA1 PB6 = CRDY** waiting for "ready" (low).
3. Host sets **DDRA = $FF** to jam the byte onto the COPS data lines.
4. COPS reads it and raises **CRDY high** (took data); host waits for that.
5. Host sets DDRA = 0 and re-enables CA1.

The model detects the jam in the port-A callback by querying
`via_port_direction(via1, 0) == $FF`, captures `via_port_output` as the command,
drives CRDY high, then drops it back to ready on release. CRDY idles low.

### Response path — `GETDATA` / `ReadCOPS`
The COPS drives port A with a response byte and pulses **CA1** (sets VIA1 IFR
bit 1); the host polls IFR bit 1, then reads ORA (reg 1, whose read clears the
flag). The model presents the byte on the port-A input pins and pulses CA1
(`via_input_c(via1,0,0,…)`), paced to host consumption by a scheduler "pump"
that only advances when IFR CA1 is clear (so it never overruns an unread byte).

### Reset / power-up codes
The host pulses **VIA1 PB0** low (`RSTKBD`) then high (`CLRRST`); on the low→high
edge the COPS emits its reset codes. The model replies `$80` (reset lead-in) +
`$3F` (final-US keyboard id, ≤ $DF ⇒ "connected"), and sends **no** mouse codes
(which `RSTSCAN` reads as "mouse connected"), so the ROM's COPS self-test passes
with no errors.

### Mouse
`#111 ennn` (`$78`–`$7F`) enables mouse interrupts at `nnn × 4 ms`. Once
enabled, the COPS reports `$00 dx dy` every interval **even when idle** — the
boot ROM's input loop (`WT4INPUT`/`COPS0`) blocks on `ReadCOPS`, so these
periodic reports are what keep the boot/monitor loop alive between keypresses.
The model schedules a recurring "mouse" event and enqueues the marker + the
accumulated `dx`/`dy` (host-injected via `cops_inject_mouse`, below).

Unlike the Mac (whose CPU reads raw mouse **quadrature** off the VIA/SCC), the
Lisa mouse plugs into the COPS, which decodes the pulse edges itself and reports
**cooked signed `dx`/`dy` deltas** to the CPU (HM §6.6.3, §11.4). So the
faithful injection point is the delta the COPS would have counted — not
quadrature — which is exactly what `cops_inject_mouse` feeds.

### Host input injection
`cops.h` exposes two entry points so the host can drive the guest:
- `cops_inject_key(code)` — queue a raw COPS scancode (a keyboard key, or the
  mouse-button keycode `$86` down / `$06` up) onto the same response FIFO the
  COPS delivers through (CA1 + port A).
- `cops_inject_mouse(dx, dy, button)` — accumulate `dx`/`dy` (reported on the
  next mouse tick, as the real COPS reports counted edges) and, on a button-state
  change, queue the button keycode.

These are wired generically: `machine.h`'s `hw_profile_t.input_key /
input_mouse_move / input_mouse_button` hooks let the standard `keyboard` /
`mouse` object methods (`keyboard.press`, `mouse.move`, `mouse.click`) route to
the Lisa COPS (via `system_input_*` in `system.c`) instead of the default Mac
ADB/Toolbox path — which is untouched, so Mac machines are unaffected.  Verified:
keycodes and `[$00,dx,dy]` reports are delivered to and consumed by the OS's
COPS state machine, and `MouseMovement` updates the OS cursor globals.  (Note: the
Lisa boot **menu** is mouse-driven — its input loop watches for the mouse-button
code, not keyboard.)

### Clock / NMI key / power
Clock read/write (`$02`, `$1n`), set-modes (`$2x`), and NMI-key nibbles
(`$5n`/`$6n`) are accepted today; the RTC protocol and key-driven NMI land with
the input/clock work.

### CRDY (VIA1 PB6) — free-running ready/busy toggle
The host hands a command byte to the COPS in sync with `CRDY` (VIA1 **PB6**),
which the COP421 drives as a **free-running ready/busy line** — it toggles
continuously as the COPS cycles its scan loop, *not* a static level. A sender
(the boot ROM's `COPSCMD`, or MacWorks XL's own bit-bang driver) presents the
byte, spins for a CRDY **edge**, drives port A to clock it in, then waits for
the next edge and releases. We therefore toggle CRDY on a fixed period
(`cops_crdy_tick`) rather than deriving it from `DDRA`: a static level satisfied
the boot ROM but hung MacWorks, which waits for a high edge before driving the
bus. See docs/lisa.md §11.

## Bring-up status

With the COPS attached, the rev-H boot ROM completes the **entire** power-on
self-test — ROM checksum → MMU register/context tests → memory sizing →
preliminary memory test → VIA timer test → video test → **COPS keyboard/mouse
self-test (`RSTSCAN`)** — and reaches the boot/monitor idle loop, where it
processes periodic mouse reports and waits for a bootable disk or a keypress
(verified headless: the boot loop's `MouseMovement` handler is reached
repeatedly). Booting an OS from here needs the floppy controller (Step 5).
