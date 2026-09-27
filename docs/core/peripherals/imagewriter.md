# ImageWriter printer on a serial port

`src/core/peripherals/imagewriter.{c,h}` models an Apple ImageWriter-class
dot-matrix printer at the far end of one Z8530 SCC channel's cable. It is an
on-board device: a machine fits one to a port at construction and the user
does not choose it. Today the Lisa fits one to **Serial A**.

## What it does

- **Holds the port's ready line asserted.** A powered-on ImageWriter raises
  its ready line (DTR on the printer's connector); through the Macintosh or
  Lisa cable that arrives at one of the SCC's handshake inputs. The printer
  drives that input with `scc_set_input_pin` at power-on and keeps it there
  ([scc.md](scc.md), "Devices on a port"). Drivers that wait for the printer
  before transmitting therefore always find it ready. The emulated printer
  has an unlimited buffer, so it never deasserts the line for flow control.
- **Takes every asynchronous byte** the guest transmits on that channel
  (`scc_set_tx_byte_sink`) into the job in progress.
- **Ends a job after a quiet gap.** The ImageWriter protocol has no
  end-of-job marker; a real printer just prints what arrives. The model ends
  a job after `IMAGEWRITER_IDLE_NS` (5 s) of guest time with no byte, a gap no
  driver leaves inside a document. A job longer than `IMAGEWRITER_JOB_MAX`
  (8 MB) is handed over at that size and the rest continues as a new job.
- **Hands each job to the platform** through `imagewriter_sink_job` as the
  raw ImageWriter command stream, byte for byte:
  - headless writes `<print-dir>/iw-<job>.iw` (`--print-dir` or
    `$GS_PRINT_DIR`; without one the job is logged and dropped);
  - the browser downloads `iw-<job>.iw`, the way a captured LaserWriter job
    is downloaded;
  - the weak default logs and drops it.

Rendering the stream to a page (PDF) is not part of the device. The stream is
what the guest's driver sent, so any ImageWriter interpreter can render it.

## Object model: `machine.printer`

| Member | Kind | Meaning |
|---|---|---|
| `jobs` | attribute | Jobs handed to the sink so far. |
| `bytes` | attribute | Bytes received since power-on. |
| `pending` | attribute | Bytes of the job in progress (0 between jobs). |
| `last_len` | attribute | Bytes in the last finished job. |
| `flush()` | method | End the job in progress now, instead of waiting for the gap. |
| `last_job()` | method | The last finished job as text: printable ASCII kept, every other byte escaped `\xNN` (the `scc.<ch>.sent()` convention). |

A script can print, then either wait out the gap or call `flush()`, and
assert on `jobs` and `last_job()`.

## The Lisa's Serial A

The wiring comes from the Lisa OS RS-232 driver (`source-rs232`, Apple, 1983):

- The built-in ports are the on-board SCC; port A is channel A.
- For port A the driver sets `xmtrr0 := $10`: it transmits only while RR0
  bit 4 is set. On the Lisa the port's DSR input is wired to the SCC's
  `/SYNC` pin, which RR0 bit 4 reports in asynchronous mode. The printer
  therefore drives **SYNC, asserted**. (Port B uses `xmtzrr0 := $20`, CTS
  required low; no device is fitted there.)
- Opening the port resets the channel (`WR9 = $8A`); the ready level
  survives that because it is the cable's, not the chip's.

What the Lisa Office System 3.1 sends for a printed Calculator tape, as the
`lisa-imagewriter` integration row captures it (5054 bytes): a run of CAN
(`$18`) bytes that reset the printer and flush its buffer, setup codes
(`ESC E`, `ESC T16`, `ESC >`, `ESC P`, ...), then the page as bit-image
bands (`ESC F` to position the head, `ESC g` for 8-dot graphics columns).
The Office System prints text as graphics, so the stream renders exactly
with a dot-matrix bitmap interpreter.

Three model fixes were needed before the driver would print, all
chip- or board-accurate:

1. An expansion slot with no card bus-errors (the Lisa MMU), as both the boot
   ROM's `RDSLOTS` and the OS's `EXISTS_CARD` expect; a floating `$FF` made
   the OS see a card with ID `$FFF` in every slot.
2. The SCC answers across its whole chip select, `$D000-$D3FF`: the boot ROM
   uses `$D241-$D247`, the OS's driver `$D201-$D207`.
3. The SCC's WR1 enables gate the INT pin, and Reset Tx Int Pending holds a
   transmit interrupt off until another character leaves the buffer
   ([scc.md](scc.md), "Interrupt request").

The `lisa-imagewriter` row (extended tier) drives that print end to end and
asserts on `machine.printer`; it fails if any of the three fixes regresses.

## Checkpoints

The idle timer is a registered scheduler event (`imagewriter` / `idle`), so a
checkpointed event queue restores. The job buffer itself is host-side and is
not checkpointed: a checkpoint taken mid-job loses the bytes received so far.
