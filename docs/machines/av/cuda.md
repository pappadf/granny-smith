# Cuda — the AV system-management MCU

Cuda (Apple 341S0788, firmware 2.37) is Egret's successor: a 68HC05 owning
ADB, PRAM, the real-time clock, the one-second tick, power control and the
I²C bus to the video-in chips. The AV Quadras are the first Macs to carry it.
Implementation: [src/machines/av/cuda.c](../../../src/machines/av/cuda.c) /
[cuda.h](../../../src/machines/av/cuda.h). Hardware reference:
`local/gs-docs/840av_660av/docs/via1-cuda.md`.

## Behavioral, not a core — and why that is a stronger position here

The model reproduces the host-visible wire protocol rather than executing the
68HC05. The usual argument applies (the boot-time surface is small, a core
brings pin wiring, second-CPU checkpoint state and a host↔MCU timing coupling
that threatens determinism), but this family has an unusual advantage: the
firmware **has been disassembled** and its findings folded into the dossier,
so the behavioral model is written against what Cuda 2.37 actually does — not
only against the host's view of it. That is where the rejected-command list,
the exact PRAM geometry and the handshake polarities below come from.

Forked from [mdu/egret.c](../../../src/machines/mdu/egret.c) and diverged;
Egret stays where it is, since it already crosses family lines.

## Transport

VIA1's shift register (Cuda is the external shift clock) plus three port-B
pins:

| Pin | Direction | Line | Behavior |
|---|---|---|---|
| PB3 | host input | `vCudaTREQ` | Cuda's transaction request, active **low** |
| PB4 | host output | `vCudaBYTEACK` | a **level**, toggled once per byte |
| PB5 | host output | `vCudaTIP` | transaction in progress, active **low** |

The differences from Egret that matter: BYTEACK is a toggle rather than a
pulse, TIP is active-low where `sysSes` was active-high, and Cuda runs a
**sync cycle** at init that Egret has no analogue for.

## The three flows

**Host → Cuda (command packet).** The host puts the SR in output mode, writes
the first byte, then asserts TIP. Each further byte is written to the SR and
announced with a BYTEACK toggle. Negating TIP ends the packet, which is when
the model processes it.

**Cuda → host (response or unsolicited).** Cuda asserts TREQ and clocks the
attention byte into the SR. The host answers by asserting TIP; every BYTEACK
toggle clocks the next byte. **TREQ rises with the last byte** — that is the
host's per-byte "was this the last?" test. When the host terminates the
transaction, Cuda clocks one extra **idle acknowledge** byte, which
`CudaMgr`'s `@waitIdleAck` spin-waits on.

**The sync cycle (`CudaInit`).** With the bus idle, the host asserts BYTEACK
alone. Cuda asserts TREQ and clocks a byte; when BYTEACK rises it negates TREQ
and, after a delay, clocks the idle acknowledge. The cycle's documented
purpose is to silence every asynchronous source (autopoll, RTC, power), so the
model disables them here.

### The delay is load-bearing

Cuda's idle acknowledge arrives ~25 µs after TREQ negates, and the host
*clears the SR interrupt first*. Pushing the byte synchronously means the host
consumes it before it is looking, `CudaInit` never sees its acknowledge, and
the ROM reaches `DeadCuda` and plays death chimes. So Cuda-initiated bytes go
through a scheduled delayed push.

The mirror-image hazard: a *stale* delayed push must never land inside the
next transaction, or it sets the SR interrupt out of phase and garbles the
byte stream. The polled `CudaRdXByte` path (used for the very early PRAM reads,
before the Cuda Manager exists) legitimately consumes an in-flight byte as its
idle acknowledge and moves straight on. Any new bus activity — a shift-out, or
starting a fresh response — therefore cancels whatever push is pending.

## Command surface

Response packets are `[attn][pktType][flags][cmd]` then data; error packets
are the 5-byte `[attn][errorPkt][code][pktType][cmd]` (the host reads the
fifth byte only for `errorPkt`).

Implemented: RdTime/WrTime against the RTC, RdPram/WrPram and Rd6805/Wr6805
over the PRAM window, APoll/SetAutopoll, Wr1SecMode, Reset, and accept-and-log
for the rest.

**Twelve of Apple's pseudo-commands are rejected by this firmware** —
`$04 $05 $06 $0F $15 $17 $18 $1C $1D $1E $1F $20` — returning an `errorPkt`
with code 2, as does anything above `MaxPseudoCmd`. A model must *reject*
them, not implement them; the dispatch table in the firmware is where that
list comes from.

**PRAM is one 256-byte page.** A nonzero address high byte is rejected with
error code 4 — again from the firmware, which builds its access stub in RAM
and range-checks the page.

## Interrupt masking

Cuda transactions mask only to **IPL 3**, not 7 (`CudaMgr.a <LW5>`, Radar
#1059613). A model that assumes atomicity at IPL 7 starves the DMA, DSP and
MIDI interrupts that live above it. Nothing in the emulator masks on Cuda's
behalf, so this is satisfied by construction — recorded here because it is the
kind of thing a future "optimization" would break.

## Not modelled

The I²C pseudo-command `$22` (`RdWrIIC`) accepts and acknowledges but has no
Philips chips behind it, matching the video-in scope decision. Autopoll uses
the shared ADB device model rather than reproducing the firmware's polling
state machine. Power-off logs rather than acting.
