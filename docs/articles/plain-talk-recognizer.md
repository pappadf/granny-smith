# Inside Apple's Casper: the PlainTalk speech recognizer, end to end

In 1993 the Quadra 840AV and Centris 660AV became the first Macintosh
computers with an on-board digital signal processor, and Apple used it
to ship something no desktop machine had done before: a
speaker-independent, continuous-speech, always-listening command
recognizer. The computer sat there all day, and when you said
"Computer, open the Trash," the Trash opened. This article is the
complete anatomy of that system — sold as PlainTalk, built under the
codename Casper — from the electret microphone to the action that runs
when a phrase is understood: every processing stage, every algorithm,
every constant, and where each one lives.

Two 1993 constraints shape everything that follows. The AT&T DSP3210
does all the *signal* mathematics in real time, so the 68040 never
touches an audio sample. The 68040 does all the *probability*
mathematics in integer arithmetic — byte-table lookups and 16-bit
adds — so recognition fits in the spare cycles of a 40 MHz CPU. The
recognizer is a chain of small, deliberate design decisions that keep
those two contracts, and the chain is short enough to describe
completely.

**Contents**

1. A Mac that listens — what Casper is, the recognition strategy, the
   hardware, the software inventory, the master data flow, and how
   this article knows what it knows
2. From air to numbers: the capture path
3. The DSP environment: tasks, modules, sections, FIFOs
4. The front end: eleven modules every ten milliseconds
5. Crossing to the host: the ptfe driver
6. What English sounds like: the acoustic models
7. The arithmetic of listening: scoring and search
8. Grammar, rules, actions, feedback
9. The engine's error codes

Appendix A. Verification status and sensitivity · References

## 1. A Mac that listens

### 1.1 Casper: what it is, and where it came from

Apple began investing in speech recognition around 1990, hiring
Kai-Fu Lee from Carnegie Mellon, where his PhD system Sphinx [4] had
just become the first large-vocabulary, speaker-independent,
continuous-speech recognizer built on hidden Markov models. Lee led
Apple's Speech and Language Technologies group; the recognizer his
team built was codenamed **Casper**, demonstrated by John Sculley on
*Good Morning America* in March 1992, and shipped in 1993 as
**PlainTalk** with the first AV Macintoshes (Quadra 840AV / Centris
660AV) — the first Macs with an on-board DSP [1].

Casper is:

* **Speaker-independent** — no per-user training. The acoustic models
  were trained on voice samples from over 500 adult North American
  English speakers [5].
* **Continuous** — you speak naturally, without pausing between words.
* **Command-and-control, not dictation** — at any moment it listens
  for a bounded set of phrases (a few dozen), defined dynamically by
  "speech rules". It cannot transcribe arbitrary text.
* **Name-gated** — to avoid triggering on background conversation, a
  command must normally begin with the computer's spoken name
  ("Computer, …"), or listening is gated by a push/toggle "attention
  key". Crucially, as §8.2 shows, the name is *not* a special
  detector — it is simply the first word of every grammar path,
  recognized acoustically like any other word.

### 1.2 The recognition strategy in one page

Every automatic speech recognizer of this family answers one question:
*given the audio, which of the currently-legal word sequences is most
probable?* Casper's pipeline is the classic early-1990s discrete-HMM
recipe, and each link of the chain is one section of this article:

1. **Feature extraction** (§4): every 10 ms, the DSP compresses the
   recent audio into a small vector describing the *shape of the
   short-term spectrum* — what distinguishes an "ah" from an "ee"
   from an "sh" — while discarding pitch, phase, and absolute
   loudness, which distinguish *speakers*, not *words*. Casper uses
   LPC-derived, frequency-warped cepstral coefficients plus energy
   and their time-derivatives.
2. **Vector quantization** (§4.11, §6): each feature vector is
   snapped to the nearest pre-trained codebook entry, so 10 ms of
   speech becomes a handful of small integers — *codewords*.
   Probability distributions over integers are lookup tables of
   bytes: all a 40 MHz integer CPU can afford per frame.
3. **Acoustic models** (§6): each speech sound, in context, is a
   tiny 3-state hidden Markov model; words are chains of phone
   models, phrases are chains of words.
4. **Search** (§7): recognition is finding the path through the
   currently-active grammar whose models best explain the observed
   codewords — a Viterbi dynamic-programming search [6], kept
   tractable by beam pruning: paths that fall too far behind the
   current best are abandoned.
5. **Grammar and interpretation** (§8): the legal phrases are
   generated dynamically from speech rules — including one rule per
   file in the Speakable Items folder, whose *filenames are the
   vocabulary*. A recognized phrase triggers its rule's action; a
   parse through "rejection" arcs yields "Pardon me?"; a collapsed
   search yields — by design — no reaction at all.

Everything in this system is engineered around the two constraints of
the introduction: the DSP does all signal mathematics in real time so
the 68040 never touches a sample, and the 68040 does all probability
mathematics in integer arithmetic on bytes and 16-bit scores.

### 1.3 The machine

```
                    ┌────────────────────────────────────────────┐
                    │              Quadra 840AV                   │
   PlainTalk mic ──▶│ Singer codec ──▶ PSC ──▶ system RAM        │
   (electret,       │  (A/D, gain      (DMA)      ▲   ▲          │
    100–200 mVpp    │   ladder,                   │   │          │
    at the jack)    │   24 kHz)                   │   │          │
                    │                             │   │          │
                    │        68040 @ 40 MHz ──────┘   │          │
                    │        (RTM, recognizer,        │          │
                    │         rules, UI)               │          │
                    │                                 │          │
                    │        AT&T DSP3210 @ 66.67 MHz ┘          │
                    │        (front end, under the ARTA kernel)  │
                    └────────────────────────────────────────────┘
```

* **The PlainTalk microphone** is a powered electret designed for the
  AV Macs; the input stage expects roughly **100–200 mVpp** at the
  sound-input jack, and Apple's documentation is blunt that the
  speech-recognition microphone is required for best results
  ([1] p. 318; [dsp3210-board.md](../reference/machines/av/dsp3210-board.md) §3.8). This contract matters enormously in
  practice: the whole acoustic-model stack was trained through this
  microphone and this analog path, so both the *level* and the
  *spectral coloration* of the input channel are baked into the
  models (§2.1).

* **The Singer codec** ("Zio" to the driver stack) digitizes at
  **24 kHz, stereo, 16-bit**, with a host-controllable analog input
  **gain ladder: 16 positions, 0 to +22.5 dB in 1.5 dB steps**
  ([singer.md](../reference/machines/av/singer.md) §2.3) — the knob the
  recognizer's automatic gain control turns (§5.5). The codec, its
  serial frame bus, its registers and its boot-time driving are the
  subject of [singer.md](../reference/machines/av/singer.md); this
  article covers only what the recognizer does with them.

* **The PSC** (Peripheral Subsystem Controller) moves the codec's
  output into a double buffer in system RAM and raises the frame
  interrupts that pace the DSP kernel — the sound DMA engine is
  [psc.md](../reference/machines/av/psc.md) §3.5's subject (see also
  [dsp3210-board.md](../reference/machines/av/dsp3210-board.md) §3.4).

* **The AT&T DSP3210** is a 32-bit *floating-point* DSP (a rarity in
  1993) [3]: 8 KB of on-chip RAM, and a multiply-accumulate datapath (the
  "DAU") with a deeply pipelined architecture whose documented 1-, 2-,
  3- and 4-instruction result latencies ([dsp3210.md](../reference/hardware/dsp3210.md)
  §3.2.1) Apple's engineers exploited on purpose — several modules
  below only compute correctly *because* of them. It shares the
  68040's memory bus; the board integration, interrupt wiring, kernel
  data area and doorbell are
  [dsp3210-board.md](../reference/machines/av/dsp3210-board.md)'s
  subject, not restated here.

* **The 68040** runs everything else: the Real Time Manager that
  administers the DSP, the acoustic scorer and Viterbi search (pure
  integer code), the language-model machinery, AppleScript actions,
  and the feedback UI.

### 1.4 The software inventory

Everything lives in `System Folder/Extensions/` except the control
panel and the vocabulary folder:

| file | key contents | role |
|---|---|---|
| **SR North American English** ("SRNAE") | 26 `dspf` DSP modules; `ptfe`:80 — the host-side front-end driver (31 KB); `loop`:21 — the acoustic scorer; `hmms`:70 — the acoustic-model component; model data: `hmmd`:70, `floc`:70, `cbtb`:70, `vqtr`:2000–2007, `Bsdc` seeds; a 408,486-byte data fork of model tables | the language-specific acoustic package: everything trained on North American English |
| **PlainTalk™ Speech Recognition** ("PTSR") | Component Manager components: `bldr`:10 (language-model builder), `dict`:18 + `look`:17/18/19 (pronunciation dictionary + letter-to-sound rules), `srch`:20 (the search engine), `rslt`:30 (result objects), `rule`:19; a 460,800-byte data fork (the spelling dictionary) | the language-independent recognition engine |
| **SR Monitor** | the feedback application: the windoid, 83 `PICT` animation frames, `fdbk` component | the on-screen presence: ear icon, level meter, recognized text |
| **System Speech Rules**, **My Speech Macros** | AppleScript `rule` resources | the global grammar: Speakable Items enumeration, menu commands, file sharing, "acknowledge failure of understanding" ("Pardon me?"), user macros |
| **Speech Setup** control panel | — | Recognition on/off, the Tolerant↔Strict slider, the computer's name, the attention key, feedback options |
| `Apple Menu Items/Speakable Items/` | 16 zero-byte alias files | **the file names are the vocabulary**; each item's action is "resolve the alias, open the target" |

A note on code style that recurs through §5–§7: every host-side piece
(ptfe, srch, loop, hmms, bldr, rslt) is a Component Manager component
on the outside — but only the outermost selector dispatch is Component
Manager. Inside, they are C++ objects with vtables; a special
selector (901) hands the caller the real object, and from then on
everything is `JSR ([offset,A0])` virtual calls. The DSP side, by
contrast, is bare-metal assembly with hand-scheduled pipelines.

### 1.5 The master data flow

One 10 ms frame's journey, with sizes (details in the sections given):

| stage | in → out | § |
|---|---|---|
| Singer A/D | analog → 240 stereo int16 pairs @ 24 kHz | 2 |
| kernel input convert | pairs → 240 mono floats ±1.0, DC-blocked | 2 |
| AnalogGC | 240 → 240 (levels metered; gain requests) | 4 |
| PPSRC | 240 @ 24 kHz → 160 @ 16 kHz | 4 |
| WinAuto | 160 samples → 15 autocorrelation lags | 4 |
| LpcCep | 15 lags → 13 floats (pseudo-log E + 12 cepstra) | 4 |
| FreqWp | 13 → 13 (frequency-warped) | 4 |
| EndPoint | 13 → 13 (+ utterance open/close flags) | 4 |
| EnvNorm | 13 → 26 (corrected + uncorrected copy) | 4 |
| VecGen6 | 26 → 63 (static + Δ + ΔΔ + energies + 2 copies) | 4 |
| VQ | 63 floats → 8 × {codeword, distance} | 4 |
| UttRjct | 16 → 6 codeword indices (+ speech verdict flag) | 4 |
| EndPad → FIFO | 6 words = 24 bytes per frame | 4, 5 |
| ptfe drain | 24 bytes → **6 bytes** (low byte of each index) | 5 |
| loop/21 scorer | 6 bytes → per-state costs for 560 models | 7 |
| srch/20 | costs → surviving grammar path | 7 |
| rules layer | path → action + feedback | 8 |

Total decision latency budget: 30 ms of deliberate lookahead in
feature extraction, 400 ms of silence to declare the utterance over,
300 ms of tail padding, then a sub-second search on the 68040 (§8.7).

### 1.6 Method: how this article knows what it knows

Nothing here is folklore. Every claim was established by reverse
engineering the shipped software: all 26 DSP modules disassembled and
the 12 live ones decoded (most verified instruction-by-instruction
against independent reference implementations), the host driver and
recognition engine disassembled and decoded, and the whole chain
traced live, frame by frame, on both accepted and rejected
utterances [7]. Where a claim rests on such tracing rather than on
published documentation, it is marked *observed*; where it could not
be fully confirmed, it is marked *inferred — unverified*. Per-module
verification status is stated in place; Appendix A summarizes.

## 2. From air to numbers: the capture path

This section follows the audio from air pressure to the first
recognizer-owned buffer: the analog input stage, digitization, DMA
into RAM, and the DSP kernel's conversion into the floating-point
world in which the whole front end operates.

### 2.1 The microphone and the level contract

The AV Macs shipped with the PlainTalk microphone, a powered electret
designed to deliver roughly **100–200 mVpp at the sound-input jack**
for normal speech at normal distance. This number is a *system
contract*, not a suggestion, for three compounding reasons:

1. **The A/D gain ladder has limited range** (±22.5 dB in 1.5 dB
   steps, §2.2). A source far outside the window either clips at the
   converter or drowns in the noise floor before the automatic gain
   control can compensate.
2. **The recognizer's AGC reacts to the signal it sees** (§4.3 and
   §5.5): a source 6–12 dB hot causes the gain loop to wind the analog
   gain down *during* the first utterances, distorting their dynamics.
3. **The acoustic models were trained through this microphone and this
   analog path.** Both the level and the *spectral coloration* of the
   channel are baked into the trained distributions. Feeding the
   system audio from a different channel (a flat digital source, a
   phone recording) presents spectra the models never saw —
   measurably shrinking the margin every word must clear in the
   search (Appendix A.2).

### 2.2 The Singer codec

The Singer sound codec (the driver stack calls the device "Zio")
digitizes the microphone at **24,000 samples/s, 16-bit, stereo** (the
mono microphone appears duplicated on both channels). On the input
side it provides:

* the **analog input gain ladder**: bits 16–19 of the codec control
  longword select one of 16 gains, 0 to +22.5 dB in 1.5 dB steps
  ([singer.md](../reference/machines/av/singer.md) §2.3). The system
  initializes it mid-ladder (+7.5 dB), and thereafter it is the
  actuator of the recognizer's AGC loop: the DSP's AnalogGC module
  measures, the host driver steps the ladder ±1 at a time (§5.5).
* the hardware sample clock that ultimately defines the 10 ms frame:
  240 samples at 24 kHz.

An implementation detail with outsized consequences for anyone
reproducing the machine, documented here because it is invisible on
real hardware: the front end's mathematics assumes a real analog
input, i.e. one that is **never digitally silent**. A real codec
always delivers at least thermal/quantization noise. Two downstream
stages divide by or normalize with frame energy; mathematically exact
zero frames drive them to infinities that permanently poison the
utterance detector's adaptive mean. Any faithful reproduction of this
system must include an A/D noise floor. (*inferred — unverified*: the
claim of poisoning follows from the arithmetic; the shipped stack
never encounters digital silence through the analog jack.)

### 2.3 DMA into system RAM: the PSC

The PSC moves the codec's output into a **double buffer** in system
RAM (`sndInBase`, [singer.md](../reference/machines/av/singer.md) §2.6;
the engine is [psc.md](../reference/machines/av/psc.md) §3.5's
subject) — stereo int16 pairs, exactly as produced. It also generates
the frame-rate interrupt that paces the DSP kernel
([dsp3210-board.md](../reference/machines/av/dsp3210-board.md) §3.4).
On AV machines *all* sound — input and output — flows through this
path and the DSP; there is no legacy sound path (the ROM skips the
classic `.Sound` driver entirely on DSP machines).

### 2.4 Into the float world: the kernel's input conversion

The DSP kernel's standard-sound input machinery (Apple's converter
modules, loaded from ROM/Enabler resources — not part of the
recognizer) runs every frame before any recognizer code:

1. **Deinterleave** the stereo int16 pairs (the recognizer's task is
   connected to the mono standard-sound input section).
2. **Convert to floating point** with an explicit ×2⁻¹⁵ scale — so
   full-scale int16 becomes ±1.0. From here to the vector quantizer,
   *everything* is DSP3210 single-precision floating point, and the
   pipeline convention is **audio lives in ±1.0**.
3. **Remove DC** with the converter's own high-pass. (The converter is
   software-pipelined against the DSP's multiplier-result latency; its
   phase-splitting `float32(int32(x))` round trip is also the reason a
   reproduction must model the DAU's integer/float accumulator lanes
   exactly ([dsp3210.md](../reference/hardware/dsp3210.md) §3.8).)

The result, deposited where the recognizer task's first module will
read it: **240 mono floats in ±1.0, DC-free, per 10 ms frame**.

### 2.5 Three gains in series, and what is absent

Three different gain mechanisms act in series, and it pays to keep
them straight when reading the sections that follow:

| mechanism | where | granularity | who controls it |
|---|---|---|---|
| analog input gain ladder | Singer, before the A/D | 1.5 dB steps, 0..+22.5 dB | host driver (ptfe), ±1 step per idle pump, on request from the DSP's AnalogGC |
| energy normalization | EnvNorm module (§4.9) | continuous, per frame | DSP, automatic: subtracts the tracked background level from the energy feature |
| per-level correction table | EnvNorm + host adaptation (§4.9, §5.6) | 53 discrete levels × 13 features | learned at runtime from accepted utterances; persists across sessions |

Notice what is *absent*: there is no digital gain on the samples
themselves, and no cepstral-mean normalization (the standard modern
defense against channel coloration). Level is handled by the ladder +
energy normalization; *spectral shape* is handled only by the learned
correction table — which is exactly why the microphone's fixed
coloration could be (and was) left baked into the models.

## 3. The DSP environment: tasks, modules, sections, FIFOs

Apple's Real Time Architecture (ARTA — a near-superset of AT&T's VCOS
[2]) is the operating environment for the DSP. Its concepts recur
through everything below; the Real Time Manager and the kernel's own
boot are described in [dsp3210.md](../reference/hardware/dsp3210.md)
§4.1 and the host-visible board machinery in
[dsp3210-board.md](../reference/machines/av/dsp3210-board.md) §2.4 and
§3.3 — not restated here.

### 3.1 Frames and bandwidth

The **frame** is the DSP's heartbeat: one frame = **10 ms** = 240
samples at 24 kHz. Every real-time task runs once per frame, admitted
under a guaranteed processing bandwidth (GPB) budget so deadlines can
never be missed. The frame tick is the interrupt the host sends to the
DSP ([dsp3210-board.md](../reference/machines/av/dsp3210-board.md)
§3.4), and frame overruns — the cases where a task misses its
deadline — are latched and handled by the board machinery
([dsp3210-board.md](../reference/machines/av/dsp3210-board.md) §3.5);
§5.8 covers what an overrun does to *recognition* specifically.

### 3.2 Modules and sections

A **module** is one loadable DSP program (resource type `dspf`) with a
standard calling convention. Register r19 points to its **section
table**; entry *i* holds the address of section *i*'s data (`[0]` = the
code itself, then ChanIn / ChanOut / private sections in declaration
order). r18 holds the return address; r22 points to the kernel's
service table (e.g. slot 23 = `kevtCopyFIFO`).

A **section** is a named, typed buffer belonging to a module (its I/O
channels, parameters, state). The host reads and writes sections
through the Real Time Manager; modules are chained by *connecting*
one's ChanOut to the next's ChanIn. The `dspf` resource itself is a
container: a type-2 header followed by one {size, flags, bind, name}
descriptor per section, the code section carrying flags $00410021,
bind 0.

### 3.3 Overlay execution and host-resident sections

The kernel copies each module's code and (most) sections into on-chip
RAM just before running it and back out after — so at run time every
module sees its buffers at on-chip addresses, while the *authoritative*
copies live in host RAM. A few sections are deliberately left in host
RAM and accessed in place; those are the cells the 68040 shares with
the DSP (the utterance flags, parameter blocks, and the big codebooks
— §4.8, §5.3). A consequence for anyone instrumenting the real thing:
modules cannot be hooked by address, because they execute from a
moving cached copy; they can be hooked by globally-unique instruction
words.

### 3.4 FIFOs and the host interface

A **FIFO** is a kernel-managed ring buffer connecting a section to
host memory, with optional message notifications. The front end's
output crosses to the host through one (§4.13, §5.4).

The 68040 drives everything through the `_DSPDispatch` trap ($ABF5) —
NewTask / LoadModule / ConnectSections / NewFIFO / InsertTask and
~100 more selectors. DSP→host signalling is a doorbell interrupt
([dsp3210-board.md](../reference/machines/av/dsp3210-board.md) §3.3)
that ends in a **deferred task** on the 68040 — the classic Mac OS
interrupt-safe callback context, and the place the drain routine of
§5.4 runs.

## 4. The front end: eleven modules every ten milliseconds

This is the heart of the signal processing: eleven DSP modules, run in
a fixed chain once every 10 ms, that turn 240 audio samples into six
codeword integers and an utterance-boundary decision. Each module is
explained three ways: **why the stage exists** (the concept, for
readers new to speech processing), **exactly what it computes** (the
algorithm with Apple's constants), and **how it is implemented**
(sections, state, quirks). Verification status is noted per module —
"verified" means the module's actual bytes were executed against an
independent reference implementation and matched.



### 4.1 The chain and its calling convention

The host driver (§5.2) builds one DSP task, "ZioMonoDSPTask",
containing the eleven modules connected output-to-input, in this
execution order:

```
AnalogGC → PPSRC → WinAuto → LpcCep → FreqWp → EndPoint
        → EnvNorm → VecGen6 → VQ → UttRjct → EndPad
```

(A twelfth module, OutLevel, runs in a separate task on the *output*
sound path — §4.14.)

Every module is entered as a subroutine with:

| register | meaning |
|---|---|
| r19 | → section table: entry *i* at r19+4·i is section *i*'s data pointer; `[0]` = the module's own code, then its sections in declaration order (usually `[1]` ChanIn, `[2]` ChanOut, then private sections — but declaration order rules: FreqWp declares Temp before its channels) |
| r18 | return address (every module ends `goto r18` + delay-slot nop) |
| r21 | stack pointer |
| r22 | kernel service table: slot *n* lives at r22+$200+4n; the chain uses `kevtCopyFIFO` (slot 23), `kevtGetSectionSize` (43), a frame/task gate (50) |

Overlay execution (§3.3) applies to every module; the sections that
stay host-resident are called out below.

### 4.2 The numeric environment

DSP3210 single-precision floats: a 24-bit two's-complement mantissa
with implicit bit and an 8-bit exponent; a packed value equals
mant × 2^(exp−151), and exponent zero means zero
([dsp3210.md](../reference/hardware/dsp3210.md) §3.8.1). The DAU has
documented result latencies — an accumulator feeding the multiplier
is 3 instructions stale, a stored value read back 1 instruction later
returns the *pre-store* value, flags used by a branch may be 4
instructions old ([dsp3210.md](../reference/hardware/dsp3210.md)
§3.2.1) [2] — and Apple's modules are hand-scheduled *against* these
latencies; several algorithms below are only correct because of them.

Pipeline conventions: audio samples live in **±1.0** (§2.4); feature
values are O(1); the pseudo-log energy is O(−300..0) (§4.6).

### 4.3 AnalogGC — analog gain control probe

*(119 instructions; decoded)*

**Why.** Microphone level varies with speaker, distance, and room.
The only level actuator with real headroom is the *analog* gain ladder
in the codec (§2.2), and only the host can touch codec registers. So
the first module in the chain is a **meter**: it inspects each frame
and, when the level is persistently wrong, posts a request for the
host to step the analog gain.

**What it computes.**

* Copies its 240-sample frame from ChanIn to ChanOut **unchanged**
  (the "tap" idiom: the copy is the work; a discarded multiply rides
  along in the same instruction).
* Computes the frame **energy** (a 240-term dot product of the frame
  with itself), and maintains in its State section: the running
  **maximum** frame energy, the running **minimum** frame energy, and
  the running minimum per-sample square.
* Compares these against fixed thresholds from its constant pool and
  writes **+1 or −1 into `AnGCParm[2]`** — "please step the analog
  gain up / down". The host acknowledges by zeroing the cell (§5.5).
  Cells `AnGCParm[0..1]` are host-initialized pointers to the
  utterance flags (§4.8), so gain decisions can respect utterance
  state.

**Implementation notes.** Sections: ChanIn, ChanOut, `AnGCParm` (the
request mailbox, host RAM), `State`. The min/max tracking uses the
DSP's conditional-store forms (`ifagt`/`ifalt` writing through a
pointer) — one of the ISA corners this chain exercises.

### 4.4 PPSRC — polyphase sample-rate conversion, 24 → 16 kHz

*(150 instructions; configuration verified live)*

**Why.** The feature analysis wants **16 kHz**:
telephone-band-plus speech (0–8 kHz) captures the formants and
fricative energy that distinguish words, at two-thirds of the compute
of 24 kHz. The codec runs at 24 kHz, so the chain resamples by the
rational ratio **2/3**. The textbook method — and this module — is
polyphase resampling: conceptually, upsample ×2 (insert zeros),
low-pass filter below the new Nyquist to suppress images/aliases, then
keep every third sample. The polyphase trick evaluates only the filter
taps that actually land on kept samples, making cost proportional to
the *output* rate.

**What it computes.**

* Ratio **L/M = 2/3**; FIR **order 13**, i.e. 26 total coefficients
  (order × L), organized as 2 phases.
* 240 samples in per frame → **160 out** (10 ms at 16 kHz).
* The coefficients are **not stored in the module**: the host computes
  them at startup, in SANE extended precision — a windowed-sinc design
  with cutoff 2π/max(L,M) — writes them as IEEE singles into locked
  memory, and passes a physical pointer in the module's `PolyParm`
  parameter block. The module converts IEEE → DSP32 with the `dsp()`
  instruction as it loads them.

`PolyParm` layout, as configured live: `{L=2, M=3, order=13,
inBytes=960, outBytes=640, ratio=1.5 (IEEE), coeffPtr, count=26}`.

**Implementation notes.** The recognizer's *other* rate converter,
`SRC2416` (a bigger direct FIR), ships in the same resource file but
never executes in this configuration — the front end builds its graph
with PPSRC.

### 4.5 WinAuto — pre-emphasis, windowing, autocorrelation

*(121 instructions; verified against an independent NumPy reference,
r = 0.99997)*

**Why — three classic steps in one module.**

1. **Pre-emphasis.** Voiced speech energy falls roughly 6 dB/octave
   with frequency (the glottal pulse's spectrum), so the high formants
   that matter for discrimination ride far below the low ones. The
   standard fix is a one-tap high-pass differencer
   `y[n] = x[n] − a·x[n−1]` with a ≈ 0.95–0.98, which tilts the
   spectrum back up ~+6 dB/octave and roughly whitens voiced speech
   before spectral modeling.
2. **Windowing.** The next stage models the *short-term* spectrum, so
   the frame's edges must be tapered — otherwise the implicit
   rectangular window smears spectral detail (leakage). A
   raised-cosine-family window (Hamming-like) is applied.
3. **Autocorrelation.** The LPC analysis to follow (§4.6) needs, as
   its sufficient statistics, the first p+1 lags of the frame's
   autocorrelation `R[k] = Σₙ y[n]·y[n−k]` — by the Wiener–Khinchin
   relation these encode the power spectrum's envelope.

**What it computes.** Per 160-sample frame (10 ms @ 16 kHz):

1. **Pre-emphasis** with **a = 0.96995**, *in place*, carrying the
   last sample across frames (so the filter is continuous across
   frame boundaries).
2. **Window generation on the fly**: instead of a 160-entry table, the
   module *generates* its window with the second-order cosine
   recurrence `c[n+1] = 2cos θ · c[n] − c[n−1]`, using
   **2cos θ = 1.9985**, and applies it symmetrically, walking inward
   from both ends simultaneously (window symmetry halves the
   recurrence steps). ~8 KB of on-chip RAM makes tables expensive;
   three multiplies per two samples are cheap.
3. **Autocorrelation, 15 lags** (k = 0..14): lag k as a dot product
   over N−k terms.
4. **Lag window**: multiplies R[0..14] by a fixed 15-entry taper
   (0.2916, 0.2914, 0.2908, … 0.2559 — smoothly decaying). A lag
   window slightly widens every spectral peak the LPC model will fit
   ("bandwidth expansion"), which regularizes the analysis against
   pathologically sharp resonances; its overall scale is irrelevant
   (LPC is scale-invariant; the energy path carries level).

**Implementation notes.** Sections: ChanOut (in/out in place — there
is no ChanIn; PPSRC writes straight into its buffer), State (carries
x[−1]), Temp. The in-place pre-emphasis reads x[n−1] one instruction
*after* overwriting it — correct only because a DSP3210 store is
invisible to a read issued in the next instruction (latency-1 store
shadow). This is deliberate hand-scheduling, and it is the sharpest
example of why the front end is inseparable from the DAU pipeline
model ([dsp3210.md](../reference/hardware/dsp3210.md) §3.2.1).

### 4.6 LpcCep — linear prediction to cepstrum

*(165 instructions; verified: cepstra match a double-precision
reference to 3.4e-5 on noise, ≤ 1.6e-3 worst case)*

**Why — LPC and cepstra in plain terms.** Linear predictive coding
models each speech sample as a weighted sum of the previous p samples:
`x̂[n] = Σⱼ a[j]·x[n−j]`. The best-fitting weights `a[1..p]` define an
all-pole filter 1/A(z) whose frequency response follows the
**spectral envelope** of the frame — the formant structure that
identifies vowels and consonants — while ignoring the fine harmonic
structure (pitch). The weights are computable in O(p²) from the
autocorrelation lags via the Levinson–Durbin recursion, which also
yields the prediction-error energy E (how much of the signal the model
could *not* explain — a whiteness/flatness measure).

Raw LPC coefficients make poor features (they interact nonlinearly),
so they are converted to **cepstral coefficients** — the coefficients
of the log-spectrum's Fourier series. Cepstra are the classic ASR
feature: compactly ordered (low coefficients = broad spectral shape,
higher = finer detail), approximately decorrelated, and a *truncated*
cepstrum is a smoothed spectral envelope. For an all-pole model the
cepstrum follows from the `a[j]` by a cheap recursion — no transform
needed.

**What it computes.** Input: R[0..14]. Output: 13 floats — `out[0]`
an energy measure, `out[1..12]` cepstral coefficients c₁..c₁₂.

1. **Levinson–Durbin, order 14** (all 15 lags used): the standard
   recursion — for each order i, reflection coefficient
   kᵢ = (R[i] − Σⱼ aⱼ·R[i−j]) / E, coefficient update
   a[j] ← a[j] − kᵢ·a[i−j], energy update **E ← E·(1 − kᵢ²)**. There
   is **no stability clamp** on k anywhere — safe for genuine
   autocorrelation input (|k| ≤ 1 by construction), noteworthy only
   because nothing defends against corrupted upstream data.
2. **The division** kᵢ = num/E — the only divide in the whole chain —
   is implemented as the DSP's `seed()` reciprocal estimate (a
   bit-complement trick giving a scale-free first guess) refined by a
   short fixed polynomial. Measured: relative error ≤ 6.1e-6 across
   50 decades of denominator; `x/0 → ±3.4e38`; `0/0 → 0`.
3. **LPC → cepstrum recursion**, truncated at 12:
   `n·c[n] = n·a[n] + Σₖ₌₁ⁿ⁻¹ (k·c[k])·a[n−k]`, keeping ĉ[n] = n·c[n]
   internally and producing c[n] = ĉ[n]·(1/n) with an exact reciprocal
   table (1/2 … 1/12). Only a[1..12] feed the recursion; orders 13–14
   exist to sharpen the fit and the E estimate.
4. **The energy element**: `out[0]` is a **pseudo-logarithm of E₁₄**
   (the order-14 prediction-error energy), computed from the float's
   exponent and mantissa fields:
   `out0 = P(m) + 11.3052·e + E·m·(1+m³+m⁶) − 15.2018` where
   E = m·2^e, m ∈ [0.5,1), and P is a fixed polynomial in m. It is
   log-*like* — +11.305 per octave in the exponent — but **folded**:
   P is non-monotone within each octave (~±7 wiggle) and jumps ~13 at
   octave boundaries. It is a coarse energy measure, and it is what
   shipped (the constant pool even contains an unused ln 2 — the
   routine reads like a degenerated true log₂). Two anchor values:
   digital silence (E = 0) → **−1495.5** exactly; normal speech at
   pipeline levels → around −265.

**Implementation notes.** Sections: ChanIn (15), ChanOut (13), Temp
(40 longs: previous-order coefficient copy, a[1..14], ĉ[1..12],
energy-log scratch). Heavily software-pipelined against the DAU
latencies; the divide and the pseudo-log were both verified by
tracing the actual instruction sequence on a cycle-exact DSP3210 core.

### 4.7 FreqWp — cepstral frequency warping

*(90 instructions; verified: the warp matrix identified to 2.3e-7)*

**Why.** Human hearing resolves low frequencies much more finely than
high ones (the mel scale). Recognition accuracy improves if the
feature axis matches: two spectra that differ around 500 Hz should
look more different to the models than two that differ near 6 kHz by
the same amount. Casper implements this with the Oppenheim all-pass
warp [8]: a change of frequency variable applied *directly to the
cepstrum* as a fixed linear map — no FFT, no filter bank.

**What it computes.** `out = M · in`, where M is exactly the 13×13
matrix of the bilinear cepstral warp with **α = 0.6**: column n holds
the first 13 power-series coefficients of `Â(z)ⁿ`,
`Â(z) = (z⁻¹+α)/(1+α·z⁻¹)`, with a final ×**0.64** (= 1−α²) row
normalization. α = +0.6 at 16 kHz is the mel-approximating direction:
the warped axis spends 4× more resolution at DC than at Nyquist
(slope 0.25 at DC, 4 at Nyquist).

The energy element passes through to output slot 0 only — column 0 of
M is the unit vector, so the (huge) pseudo-log energy never
contaminates the warped cepstra.

**Implementation notes.** Stateless and linear (verified bit-identical
warm vs. cold). M is never stored: the module runs two interleaved
backward α-recursions plus six passes of a two-element update ladder,
computing the warp in O(13²) multiply-adds in place. Section-table
gotcha: FreqWp declares Temp before its channels, so section [1] is
Temp and [2]/[3] are ChanIn/ChanOut — a reminder that the ABI's slot
order is *declaration* order, not convention.

### 4.8 EndPoint — the utterance detector

*(136 instructions; verified: reproduces its decision rule exactly on
synthetic input)*

**Why.** The search engine consumes *utterances*, not an endless
stream: it needs to know when speech starts (so it can begin
collecting frames) and when it ends (so it can finalize the parse).
The detector must adapt to the room: "unlike the recent background"
is the only robust definition of speech onset across environments.
Casper endpoints **in the feature domain** — on the cepstra, not the
waveform — so "sounds different from the background" means
*spectrally* different, not just louder.

**What it computes.** Runs on the 13-element vector each frame (it
sits *before* EnvNorm):

1. Maintains an IIR **running mean of the 12 cepstra**:
   `mean ← 0.49·mean + 0.0022·x` per frame — a slowly-adapting
   portrait of "what the background sounds like".
2. Computes the **squared Euclidean distance** of the current cepstra
   from that mean, adds a 3-frame history of the same, and subtracts
   the threshold **1.6**. Positive score ⇒ "this frame does not look
   like the background".
3. A two-phase counter state machine on the score's sign:
   * onset phase: **7 consecutive positive frames** (70 ms) ⇒ set
     `BeginEnd[0] = 1` — **the utterance opens**;
   * end phase: **40 consecutive negative frames** (400 ms) ⇒ set
     `BeginEnd[1] = 1` — **the utterance closes**.

The 400 ms closing rule has a user-visible corollary: any pause inside
a command that reaches ~400 ms splits it into two utterances, neither
of which matches the `<name> <command>` grammar. ("Computer, [long
pause] open the Trash" fails for exactly this reason.)

**Implementation notes — the shared-memory boundary.**

* The `BeginEnd` section is **host-resident**: the 68040 and three
  other DSP modules (AnalogGC, EnvNorm, UttRjct — each is handed
  pointers to these cells in its parameter block) all observe
  utterance state by shared memory. There are no begin/end messages
  anywhere in the system.
* `BeginEnd[2]` is a **suppress pointer**: if non-null and the word it
  points to is non-zero (while no utterance is open), the module
  writes a "suppressed" code into its state and returns without
  computing. The host aims this pointer at the *output* level
  monitor's activity flag (§4.14) — **the endpointer is disabled
  whenever the Mac itself is producing sound**, so the recognizer
  never hears the machine's own beeps and spoken replies. (The
  attention-key "standing by" state stops listening further upstream,
  by halting the input DMA.)

### 4.9 EnvNorm — energy normalization and adaptive channel correction

*(278 instructions; verified on the main paths)*

**Why.** Two normalizations remain before the features are comparable
to the trained models:

1. **Level**: the energy element still reflects absolute input level.
   The models want "energy above the current background", not "energy
   above an arbitrary zero".
2. **Channel/speaker drift**: microphone placement, room, and speaker
   shift the whole feature vector in ways that correlate with level.
   Casper's answer — 1993-vintage and rather clever — is a learned
   **per-level additive correction**: a table, indexed by how far the
   current frame's energy sits above background, whose rows are
   correction vectors added to the features. The table is re-estimated
   on the fly from utterances the system accepted (§5.6), and
   persists across sessions.

**What it computes.**

1. **Background tracking**: `bg ← 0.98995·bg + 0.01·c₀` (time constant
   ≈ 1 s), **frozen while an utterance is open** (so speech does not
   drag the background estimate up); output energy `out[0] = c₀ − bg`.
2. **Energy index**: `idx = clamp(round(4.3429·out[0]), 0, 52)` — a
   53-level ladder of "how far above background". (4.3429 = 10/ln 10;
   with the pseudo-log's 11.3-per-octave scale one level ≈ 0.06 octave,
   so the ladder spans about one octave above background.)
3. **Correction**: `out[0..12] += correction[idx][0..12]` — row `idx`
   of the host-supplied 53×13 IEEE-float table, converted with `dsp()`
   on each access.
4. **Duplicate**: the vector is also copied **uncorrected** to
   ChanOut[13..25]. Downstream, the two big "rejection" codebooks
   quantize this raw copy (§4.11) — the speech/noise verdict should
   not depend on the learned correction.
5. **Adaptation bookkeeping** (only while the host has armed a
   histogram, and gated by the utterance flags): per-frame
   `histogram[idx]++` and a per-level running **sum of the 12 warped
   cepstra** into the armed accumulator row. Up to **20 stages**
   (utterances) with keep/discard flags, capped at 1500 total frames;
   the host consumes these to re-estimate the correction table
   (§5.6). One decoded subtlety: the accumulator rows store sums with
   a one-slot shift (row[k] += in[k+1], energy excluded) — a
   consequence of the store pointer trailing the read pointer in the
   pipelined loop; the host-side reader matches it.

`EnvParms`, the host protocol block, carries: the stage counter,
pointers to the utterance flags and to UttRjct's verdict cell, 20
histogram pointers, 20 accumulator pointers, the live
correction-table pointer, and a staging cell where the host publishes
a re-estimated table for the DSP to latch on an idle frame (§5.3).

### 4.10 VecGen6 — the 63-element observation vector

*(verified: all 63 outputs match the specification to single
precision)*

**Why.** A single frame's spectrum says "what sound is this instant";
the *trajectory* of the spectrum says "what is it doing" — and phonetic
identity lives largely in the dynamics (formant transitions, plosive
bursts). The classic remedy: augment the static features with
**delta** (first difference over ~40 ms) and **delta-delta** (second
difference) features. This costs latency: computing a centered
difference around frame t needs frames after t, so the vector emitted
"now" describes the frame **3 frames ago** — 30 ms of deliberate
lookahead.

**What it computes.** Keeps a ring of the last 7 frames' 26-element
EnvNorm outputs. With f(k) = frame k's corrected vector (element 0 =
energy, 1..12 = cepstra) and center c = t−3:

| output slots | content | formula |
|---|---|---|
| 0..11 | static cepstra | f(t−3)[1..12] |
| 12..23 | delta cepstra | f(t−1)[k] − f(t−5)[k] |
| 24..35 | delta-delta | f(t)[k] − f(t−2)[k] − f(t−4)[k] + f(t−6)[k] |
| 36 | energy | f(t−3)[0] |
| 37 | delta energy | f(t−1)[0] − f(t−5)[0] |
| 38 | delta-delta energy | f(t)[0] − f(t−2)[0] − f(t−4)[0] + f(t−6)[0] |
| 39..50 | raw cepstra copy | current frame's *uncorrected* [13..25] |
| 51..62 | the same copy again | for the second rejection codebook |

63 elements total. The duplication is not waste: the two 256-entry
rejection codebooks each quantize their own copy (§4.11), letting the
codebook loader treat all eight books uniformly as a partition of the
input vector.

### 4.11 VQ — eight tree-structured codebooks

*(verified: 60/60 trials bit-agree with an independent model of the
algorithm, on the real shipped codebooks)*

**Why — vector quantization in plain terms.** The acoustic models on
the 68040 are **discrete** HMMs: their emission probabilities are
lookup tables indexed by small integers, because table lookups are all
a 40 MHz integer CPU can afford per frame. So the front end's final
job is to convert each continuous feature vector into those integers:
find, in each pre-trained **codebook** (a set of representative
vectors — centroids from k-means-style training), the entry nearest
the current vector, and emit its **index**. Splitting the 63
dimensions across several small codebooks (rather than one giant one)
keeps quantization error manageable with tiny tables — the
multi-stream discrete-HMM design Sphinx made standard [4].

A full nearest-neighbor scan is still too slow, so each codebook
carries a **binary decision tree**: at each node, compare *one* element
of the input against a threshold, descend left/right; the reached leaf
lists a few dozen *candidate* codewords, and only those are scanned
exactly.

**The eight codebooks.** Loaded from the `vqtr` resources (§6.7 has the
file format), the books **partition** the 63-vector exactly:

| book | dims | entries | tree depth | quantizes |
|---|---|---|---|---|
| acode | 4 | 32 | 5 | static cepstra 1–4 |
| bcode | 8 | 32 | 5 | static cepstra 5–12 |
| adcode | 4 | 32 | 5 | delta cepstra 1–4 |
| bdcode | 8 | 32 | 5 | delta cepstra 5–12 |
| xcode | 12 | 32 | 5 | delta-delta cepstra |
| p3code | 3 | 32 | 5 | E, ΔE, ΔΔE |
| noise | 12 | 256 | 8 | raw-cepstra copy 1 |
| speech | 12 | 256 | 8 | raw-cepstra copy 2 |

(4+8+4+8+12+3+12+12 = 63.) The static cepstra are split 1–4 / 5–12
because the low coefficients carry the broad spectral shape — worth
finer quantization per dimension than the higher ones.

**What it computes, per codebook per frame.**

1. **Tree descent**: `levels` scalar comparisons
   (`input[elem[node]] > threshold[node]`?) reach a leaf.
2. **Candidate scan**: the leaf's candidate list (at most 40 entries)
   is scanned with the exact distance; the best index and distance are
   kept. Distances are **squared Euclidean**, except p3code (the energy
   stream), which uses a hinge metric:
   `0.0625·max(ΔE²−0.5, 0) + ΔE′² + 0.125·ΔE″²` — absolute-energy
   mismatches smaller than √0.5 are free (levels vary; dynamics
   matter), delta-energy is weighted fully.
3. **Early exit**: each candidate carries a trained **bias**
   threshold; the scan stops as soon as the current best distance
   beats the candidate's bias ("good enough — deeper candidates won't
   help"). The early-exit branch tests a DAU flag generated four
   instructions earlier — the module is scheduled around exactly that
   latency.
4. **Output**: `{int32 global codeword index, float distance}` per
   book — 16 words per frame. (A mode flag in the parameter block
   selects pairs-with-distances vs. packed indices; the recognizer
   runs with distances on, because UttRjct consumes them.)

### 4.12 UttRjct — the speech/noise verdict

*(verified on the real core, all decision branches)*

**Why.** Doors slam; phones ring. Some detected "utterances" are not
speech at all, and the *adaptation* machinery (§4.9) especially must
not learn from them. The front end ships a purpose-built discriminator:
two extra codebooks — one trained on **noise**, one on **speech** —
both quantizing the same raw cepstra. Whichever family fits better
(has smaller quantization distance, sustained over time) tells you
what the audio was.

**What it computes.**

* **Unconditionally forwards** the six small-codebook indices
  (acode..p3code) to ChanOut — the module never gates or withholds the
  observation stream. The noise/speech *indices* stop here.
* Maintains 8-frame sliding sums of the **noise distance** (sumA) and
  **speech distance** (sumB). While an utterance is open and no verdict
  is latched:
  * if `sumA < 0.5·sumB` — the noise book fits 2× better — latch
    silently (noise: say nothing, learn nothing);
  * if `sumB < 0.5·sumA` — the speech book fits 2× better — latch and
    raise **`RjctParm[2] = 1`** ("decisively speech");
  * otherwise keep testing next frame.
* The end-of-utterance pulse resets the latch and the flag.

The flag's consumer is the **adaptation loop** (EnvNorm's host side
reads it through a pointer in `EnvParms`): only decisively-speech
utterances should update the learned correction table. Note what this
module is *not*: it is not the source of "Pardon me?" (that is the
search's rejection word, §7.6), and it cannot cause the recognizer's
silence (the codeword stream passes regardless of the verdict).

### 4.13 EndPad — framing the output to the host

*(decoded)*

**Why.** The stream to the host needs framing: the search wants a
little quiet context after the last word, and the host needs to know
where the utterance ends without parsing audio.

**What it does.**

* Copies UttRjct's 6-word (24-byte) frame into a **$2000-byte host
  FIFO** each frame while the utterance is open.
* After the endpointer closes the utterance, appends **300 ms
  (30 frames) of padding**, then a terminator frame whose **first word
  is negative** — the end-of-utterance marker travels *in-band* in the
  data stream.
* Posts a host notification: its `Message` section holds a 16-byte
  DSPMessage template whose vector the host driver filled in with the
  address of its own drain routine; the module hands it to
  `SendMessageToHost`, and the kernel's doorbell machinery does the
  rest (§5.4).

### 4.14 OutLevel and the anti-feedback interlock

A second, one-module task inserted into the *output* standard-sound
chain (the output path itself is
[dsp3210-board.md](../reference/machines/av/dsp3210-board.md) §3.7's
subject): **OutLevel** measures the speaker output's power each frame
(mean of squares against a 1e-7 threshold) and exposes an activity flag
in its parameter section. EndPoint's suppress pointer (§4.8) watches
that flag: **when the Mac is making sound, the endpointer is
suppressed**. This is the anti-feedback interlock — without it, the
system would hear its own text-to-speech confirmations and beeps as
fresh utterances.

### 4.15 One frame, end to end (worked numbers)

A frame of normal speech at proper level, mid-utterance:

```
240 floats ±0.3            (24 kHz, after the kernel's convert)
→ 160 floats ±0.3          (16 kHz)
→ R[0..14]: R[0] ≈ 2, decaying, lag-windowed
→ 13 floats: c0 ≈ −265 (pseudo-log energy), c1..c12 ∈ roughly ±2
→ warped: same magnitudes, axis stretched toward low frequencies
→ endpointer: distance vs background mean >> 1.6 ⇒ score positive
→ energy-normalized: c0 − bg ≈ a few units above 0; correction row added
→ 63-vector: statics ±2, deltas smaller, energies at slot 36..38
→ 8 codewords, e.g. (7, 3, 11, 0, 19, 2 | n=142, s=201), distances
   O(0.02..0.2) (p3code O(10³) — its metric is on a different scale)
→ 6 indices forwarded: 7, 3, 11, 0, 19, 2
```

And a frame of true silence: c0 = −1495.5 exactly, cepstra ≈ 0,
endpointer score = exactly −1.6 (distance 0 minus the threshold), and
the codewords all park on their books' "silence-ish" entries.

## 5. Crossing to the host: the ptfe driver

Between the DSP chain and the recognition engine sits one component:
**`ptfe` id 80** in SR North American English — the PlainTalk front
end driver, 31 KB of 68k. It owns everything administrative: building
the DSP task graph, feeding every module its parameters, draining the
output FIFO, packaging utterances for the engine, running the gain
loop, and persisting learned state. This is also where the system's
*silent failure modes* live, so they are documented completely (§5.8).

### 5.1 The component and its object model

`ptfe` is a Component Manager component (open/close/canDo + selectors
0–10). Internally it is a small class hierarchy: one **record per DSP
module** (a $17C-byte struct holding the module's name, FIFO refnums,
bytes-per-frame in/out, cached section pointers), subclassed per
module that needs parameter writes or service work (AnalogGC's gain
loop, PPSRC's coefficient designer, EnvNorm's adaptation, EndPad's
drain). The records form a doubly-linked list in execution order; an
optional non-record *source object* can be prepended (that is how a
test/asset player replaces the live microphone — the two input paths
converge at the DSP's input section).

Selector map (the ones that matter):

| selector | action |
|---|---|
| 8 | (re)build everything — the master builder |
| 9 | set the client sink: stores the recognizer-side factory object and fetches the **'fepf'** saved-state blob from it (§5.7) |
| 0 / 1 | start / stop listening (drain enable + monitor task + quota) |
| 10 | the **idle pump**: activates the output monitor, runs each record's service pass (gain loop, adaptation, FIFO keepers), refreshes the listening quota (~67 s per pump) |
| 6 | GetInfo: 'frat' → 100 (frame rate) |

### 5.2 Building the DSP graph

The master builder constructs the record list with each module's frame
sizes (§1.5's table), then performs, through `_DSPDispatch`:

1. Resolve the sound IO device ("Zio") via private selector 128, open
   a CPU-device connection (client name "Zio"), require RTM
   ManagerVersion 13.
2. **NewTask** "ZioMonoDSPTask"; **LoadModule** ×11 (first at the head
   position, the rest `kdspAfterInsert` relative to the previous —
   this is what fixes the execution order of §4.1).
3. **ConnectSections** pair-by-pair: each module's ChanOut to the next
   module's ChanIn (with a documented retry: if a module has no ChanIn
   — WinAuto works in place — connect its ChanOut instead).
4. Wire the graph's input: private 131 returns the device's
   standard-sound input section pair; connect to AnalogGC's ChanIn.
   Wire the output: **NewFIFO** ($2000 bytes) on EndPad's ChanOut,
   message mode 0 (all masked — the FIFO itself never interrupts).
5. Insert the task into the device's standard-sound input chain
   (private 130), position "input".
6. Write every module's parameter section (§5.3).
7. Build the **OutputMonitorTask**: NewTask + LoadModule "OutLevel",
   connect to the output standard-sound sections, insert into the
   output-monitor position, and point EndPoint's suppress cell
   (§4.8) at OutLevel's activity flag (§4.14).

Errors at build time are the *only* Real Time Manager errors that ever
reach a caller (translated to component-level codes; "insufficient
bandwidth" has its own code). After open — see §5.8.

### 5.3 The parameter sections

What the host writes into each module's host-visible sections
(complete table; pointers are physical addresses usable by the DSP):

| module.section | contents |
|---|---|
| PPSRC.PolyParm | L=2, M=3, order=13, 960, 640, IEEE 1.5, → host-designed coefficient block (26 IEEE singles, SANE-computed windowed sinc), 26 |
| AnalogGC.AnGCParm | [0..1] → BeginEnd[0..1]; [2] = the ±1 gain-request mailbox |
| EndPoint.BeginEnd | [0..1] the utterance flags themselves (host-resident); [2] = suppress pointer → OutLevel Params+$10 |
| EnvNorm.EnvParms | [0] stage counter; [1..2] → BeginEnd[0..1]; [3] → UttRjct RjctParm[2] (the speech-verdict gate for adaptation); [5..24] twenty histogram buffer pointers (53 ints each); [26..45] twenty accumulator pointers (53×13 IEEE floats each); [46] live correction-table pointer; [47] staging cell for a re-estimated table |
| VQ.VQCB | [0] tree count (8); [1..8] physical addresses of the eight relocated 'vqtr' blobs; [9] mode = 1 (emit distances) |
| UttRjct.RjctParm | [0..1] → BeginEnd[0..1]; [2] = the speech-verdict flag (DSP-written) |
| EndPad.Padding | [0..1] → BeginEnd[0..1]; [2] = 24 (bytes/frame); [3] = 720 (bytes of tail padding = 30 frames = 300 ms) |
| EndPad.Message | a 16-byte DSPMessage template: {msVector = the drain routine, 0, msData[1] = the EndPad record, 0}; plus an installed flag and a pending/ack cell |
| OutLevel.Params | left/right enables; +$10 = the activity flag EndPoint watches |

The `vqtr` codebooks are loaded with GetNamedResource, detached,
copied into **locked contiguous memory**, their seven header offsets
relocated by the block's physical base, and their physical addresses
written into VQCB — the DSP dereferences them directly (they are
among the host-resident sections of §3.3).

### 5.4 The return path — how codewords actually reach the engine

This is the system's one genuinely intricate hand-off, so step by
step:

1. Each frame while an utterance is open (plus the padding tail),
   **EndPad** writes 24 bytes to the FIFO and posts the prepared
   DSPMessage via the kernel's `SendMessageToHost` (§4.13).
2. The kernel copies the 16-byte message into its message ring and
   rings the **doorbell interrupt**
   ([dsp3210-board.md](../reference/machines/av/dsp3210-board.md)
   §3.3); the host takes the interrupt and schedules a deferred task.
3. The deferred task runs `DSPProcessMessages`, which calls the
   message's `msVector` — i.e. **ptfe's drain routine executes at
   deferred-task time**, with msData[1] handing it its own EndPad
   record. (The FIFO's own message mode stays masked; the
   FIFO-read-count polling visible in traces is only a liveness check
   in the idle pump. Architecturally: the *module* notifies, the
   *FIFO* is just the pipe.)
4. The drain loops `FIFORead` (24 bytes at a time):
   * **begin**: if no utterance object is open, ask the client factory
     (registered at selector 9) to create one;
   * **frame**: compress the six int32 words to their **low bytes**
     (lossless — codeword indices are ≤ 31) and pass the 6-byte packet
     to the utterance object's frame method;
   * **end**: a frame whose first word is negative finalizes the
     utterance (finalize / submit / dispose calls on the object);
   * around the loop, poll a device abort flag (private 151); if set,
     cancel the open utterance silently;
   * persist the open utterance object across drains; acknowledge the
     message cell so the DSP may post again.

So per utterance, the recognition engine receives exactly: **begin,
N × 6-byte codeword frames, end** — never raw audio, never features,
never energies, never the BeginEnd flags. The entire acoustic world
of the engine is six small integers per 10 ms.

### 5.5 The gain loop

On every idle pump, for the AnalogGC record: if `AnGCParm[2] ≠ 0`,
read the codec's A/D gain pair (private 139), step **both channels
±1** within 0..15 (private 137), zero the mailbox. One step per pump,
so the loop's speed is set by how often the client idles; the cells
are level-triggered, so slow pumping delays but never loses a request.
The current gain rides in the saved-state blob (§5.7), so a machine
wakes up with yesterday's calibration.

### 5.6 The adaptation loop, host side

§4.9 described the DSP half (histograms + per-level feature sums, 20
stages). The host half runs on the idle pump: when the DSP-written
stage counter reaches 20 and the staging cell is empty and the factory
seeds ('Bsdc' resources "Histogram" and "AverageVectors") are present,
re-estimate a new 53×13 correction table from the accumulated
statistics (SANE extended arithmetic), write its physical address into
the staging cell, and let the DSP latch it on an idle frame.
Utterances that UttRjct did not flag as decisively-speech are discarded
from the statistics (the keep/discard flag the DSP checks via
EnvParms[3]).

### 5.7 Persistence: the front end's acoustic memory

At client registration (selector 9), ptfe fetches from the client a
**'fepf' ("FE Parms") blob, tag 'dei0'**: 4 bytes of saved analog gain
plus the $AC4-byte AverageVectors (correction) table. At close, it
streams the current values back. This is the system's acoustic memory:
the gain ladder setting and the learned channel correction survive
restarts, per user environment.

### 5.8 The silent-failure inventory

`ptfe` originates **no user-visible feedback of any kind**: its error
sink is literally a no-op function, both DSP tasks' message action
procs are no-ops, and there is no watchdog or timeout anywhere. This
is a design stance (a background listener must be unobtrusive), and
it produces a specific catalogue of ways an utterance can vanish
without a trace — worth knowing both for debugging and as engine
archaeology:

1. Any Real Time Manager error after open — module load, FIFO trouble,
   task insert — is swallowed; a half-parameterized module can end up
   in a running graph.
2. Drain disabled (before Start / after Stop): the FIFO fills and
   wraps; the kernel's lost-data flag is never read.
3. No client sink registered: frames are read and *discarded*.
4. The client factory refuses an utterance object: same.
5. The private-151 abort flag: the open utterance is cancelled
   silently; its remaining frames drain to nowhere.
6. The client's frame handler returns an error: utterance aborted.
7. A DSP frame overrun sets the task inactive — and nothing restarts
   it: recognition is dead until Stop/Start, with no indication. (The
   overrun itself and its three categories are
   [dsp3210-board.md](../reference/machines/av/dsp3210-board.md)
   §3.5's subject; this is the recognizer-side consequence.)
8. The endpointer-suppress cell stuck non-zero (output monitor
   misbehaving): BeginEnd never rises; the host sees nothing at all.
9. If the drain vector failed to install, the DSP never notifies and
   the FIFO silently wraps.

None of these paths is the *normal* silence (that is the search
declining to deliver words — §7.6); but all of them produce the
identical user experience: the ear blinks, and nothing happens.

## 6. What English sounds like: the acoustic models

The engine's knowledge of *what English sounds like* lives in SR
North American English: 560 tiny hidden-Markov models plus the tables
that map codewords to probabilities. This section first builds up the
discrete-HMM idea for readers who have not met it, then gives every
shipped data structure, byte for byte.

### 6.1 Hidden Markov models in plain terms

A speech sound is not a point — it is a short *process*: an "ah" has
an onset, a steady middle, and a release, each lasting a variable
number of 10 ms frames. The HMM is the standard model for such a
process [6]:

* a small chain of **states** (Casper: 3 per phone — onset, middle,
  release);
* **transition probabilities**: at each frame, stay in the current
  state (self-loop) or advance — geometric duration modeling. A
  "skip" arc (state 0 → 2) lets very short realizations bypass the
  middle;
* per state, an **output distribution**: the probability of observing
  each possible codeword while in that state.

"Hidden" because you observe only the codewords, never the states;
recognition asks which *sequence of models* most plausibly generated
the observed codeword sequence, which §7's Viterbi search answers with
dynamic programming.

**Discrete** HMMs are the 1993-practical variant: because the front
end quantized each frame to codebook indices (§4.11), each state's
output distribution is just a table: for each of the 32 possible
codewords of a stream, one number. With six independent streams per
frame (static cepstra ×2, deltas ×2, delta-deltas, energies), a
state's frame probability is the *product* of six table lookups — or,
in log domain, the **sum of six looked-up numbers**. No
multiplications, no floating point: recognition-time acoustic scoring
on the 68040 is literally byte-table sums.

**Triphones.** The same phoneme sounds different depending on its
neighbors ("d" in "day" vs "d" in "dog" — coarticulation). The remedy
is context-dependent models: a separate model per (left neighbor,
phone, right neighbor) triple that the training data supports, with a
fallback to the context-independent phone model otherwise. Casper's
inventory: **560 models** covering the context-dependent and
context-independent sets plus special models.

### 6.2 The model inventory and the special indices

From the shipped tables (`hmmd` resource + the engine's use of it):

| property | value |
|---|---|
| #models | **560** (indices 0..559) |
| states per model | **3** |
| transitions per state | 3 (self, next, skip/exit) |
| streams (#cbk) | **6** — exactly the six delivered codewords |
| codewords per stream | 32 |

Special model-index ranges, used by the search's word classifier
(§7.6):

| range | meaning |
|---|---|
| 0..5 | **OOV** ("out of vocabulary") — generic speech models; a path through them means "someone said *something*, not in the grammar" |
| 6..11 | **non-vocabulary** — noise models (slams, rings, coughs) |
| 51 | **silence** |
| everything else | real phone models |

### 6.3 The triphone lookup

The mapping (center, left, right) → model index is a pre-built hash
shipped as a 13481-entry table: each entry is 6 bytes
`{model index (word); center, left, right (bytes); pad}`. The model
component's lookup method probes the hash with the 3-byte context key;
on a miss it retries with a wildcard context ($FF,$FF) — the fallback
to the context-independent model — and only then reports "no model".
The hash object is built once at prep time (load factor ~2/3) and the
raw table is discarded.

### 6.4 The observation tables — where the probabilities live

The heart of the acoustic model: per stream i (of 6), a byte table

```
Bᵢ[codeword][model][state]      32 × 560 × 3 = 53,760 bytes per stream
```

six of them back-to-back = 322,560 bytes in the SRNAE data fork. The
engine reads `Bᵢ[cw·1680 + m·3 + s]` — the tables are stored
codeword-major, so scoring one model's three states for a known
codeword touches three adjacent bytes.

**What the bytes mean.** Each byte is a **cost** — a negated log
probability — in the engine's universal unit, 1/156.26 nat (§7.1
derives it). Two further facts, measured from the shipped tables:

* Distributions are **offset-normalized**: within each
  (stream, model, state) distribution, the *best* codeword's byte is
  (almost always) 0 — the stored value is `ln(p_best / p)`, the
  log-likelihood ratio against the distribution's own favorite. That
  loses each distribution's absolute anchor `−ln p_best` (typical
  p_best ≈ 0.044), a per-model shift the search never needs because
  it only compares competing paths frame by frame.
* The **expected cost** of a frame drawn from a matching distribution
  is ≈ 47–50 units per stream — and the engine's confidence formula
  (§7.7) budgets exactly 40 units × streams × frames as the
  "well-matched" allowance. A voice whose codewords keep landing
  off-mode pays the difference on all six streams, every 10 ms; that
  running tax is precisely the margin that decides survival in the
  beam search.

### 6.5 The transition tables

Per model, 9 bytes: `{self, next, skip} × 3 states` (slots 5 and 8
are padding — state 2's "next" is the model's **exit** and it has no
skip). Same cost units. Shipped statistics: self-loops cheap
(mean ≈ 10 ≈ 0.06 nat — long stays are cheap), advance ≈ 28, exit ≈
22, and the 0→2 skip genuinely trained (minimum 4 in some models —
real two-frame realizations exist in the training data).

### 6.6 The complete data map

The 408,486-byte data fork decomposes exactly — no gaps, no slack:

```
offset      0: 13481 × 6   =  80,886   triphone hash entries (§6.3)
offset  80886:   560 × 9   =   5,040   transition costs (§6.5)
offset  85926: 6 × 53,760  = 322,560   observation tables B₀..B₅ (§6.4)
                total        408,486
```

The `floc`:70 resource is the directory: four {offset, length} records
pointing at the three sections above (the fourth slot, a
quantized/tied-distribution section, ships **empty** — the engine
contains a two-level "quantized codeword" scoring path that this
model does not use).

The resources around it:

| resource | bytes | contents |
|---|---|---|
| `hmmd`:70 | 76 | the model header: counts (§6.2), the special index ranges, the topology tag ($00063330) the scorer validates, the triphone-table entry count |
| `floc`:70 | 32 | the data-fork directory (above) |
| `cbtb`:70 | 36 | six {count=32, handle} records — the per-stream loader's table; the handles are filled at load time with B₀..B₅ |
| `vqtr`:2000–2007 | 3,144 … 51,568 | the eight VQ codebooks (§6.7) |
| `Bsdc` "Histogram", "AverageVectors" | — | factory seeds for the EnvNorm adaptation loop (§4.9, §5.6) |
| `hmms`:70 | 4,076 | the model *component* (code): loads all of the above, serves properties, performs the triphone lookup |

Loading is straightforward: no relocation (the tables are pure byte
arrays); the component detaches `hmmd` and uses its zero-filled slots
as runtime handle storage; the search locks everything and caches raw
pointers for the per-frame hot path.

### 6.7 The vqtr codebook format

Each `vqtr` resource (§4.11 explains their function):

```
+0   long   size (resource length − 4)
+4   long   vector dimension          (4, 8, 12 or 3)
+8   long   number of codewords       (32 or 256)
+12  long   tree depth in bits        (5 or 8)
+16  seven  section offsets (relocated to physical addresses at load):
            codeword pool      — entries × dim IEEE singles
            element table      — 2^depth − 1 tree nodes: which input
                                 element this node tests
            threshold table    — the node's compare value (IEEE)
            leaf start table   — per leaf: first candidate index
            leaf count table   — per leaf: number of candidates
            candidate array    — codeword indices, all leaves packed
            bias array         — per-candidate early-exit thresholds
```

Candidate-array sizes in the shipped books: 260 / 418 / 277 / 449 /
360 / 163 / 2742 / 4393 entries (acode … speech) — i.e. the average
leaf offers ~8–17 candidates, capped at 40 by the search loop.

### 6.8 The pronunciation side

For completeness — the *other* data fork: PlainTalk Speech
Recognition's 460,800-byte data fork is the **spelling dictionary**
(header {2048, 2048, 8, 224}, a letter-frequency-ordered alphabet
table, then fixed 8-byte word slots in alphabetical order), used with
the `dict`/`look` components' letter-to-sound rules ("casper.500.rdic")
to derive pronunciations for words the grammar introduces — e.g. your
Speakable Items file names — turning each into the phone (and thence
triphone-model) sequence the search needs. This is how "open the
Trash" becomes a chain of specific rows in the 560-model tables
without any per-user training.

## 7. The arithmetic of listening: scoring and search

Recognition proper happens on the 68040, in two tightly-coupled
components:

* **`srch`:20** — the search engine: owns the grammar network, the
  frame loop, pruning, the traceback word graph, and result building.
* **`loop`:21** (in SRNAE; the engine knows it by its property key
  `inlp`, "inner loop") — the acoustic scorer: turns each frame's six
  codeword bytes into per-state scores *and* runs the Viterbi
  time-update over the active model instances.

Both are Component Manager shells around C++ objects (§1.4); the
engine calls the scorer through a vtable, once per frame.

### 7.1 The score domain — integer log-probabilities

Everything is scored in **log probability**, because products of many
small probabilities underflow and multiplies are expensive; in log
domain, path probability = running *sum*, and "A is 10× likelier than
B" = a constant difference. Casper picks integer units:

> **1 score unit = 1/156.26 nat** (≈ 0.0064 nat ≈ 0.0028 bits)

derived from its own threshold builder: probabilities are converted as
`threshold(p) = trunc(ln(p)·10000.5 + 0.5) >> 9 << 3` (SANE extended at
configuration time — the *only* floating point in the recognition
path; ×10000.5, ÷512, low three bits cleared ⇒ ln(p)·156.26/8·8).
Scores live in **int16**, and since a mismatched utterance's costs
accumulate by thousands of units per second, the engine re-centers
every frame (the normalizer K of §7.2) to keep them in range.

All the engine's tunables are probabilities pushed through that
formula:

| constant | value | units | meaning |
|---|---|---|---|
| beam | 5e-9·(1+sped²), sped = 0..100 | −2992 (sped 0) … −1552 (sped 100) | per-frame pruning window below the frame best; **sped is the Speech Setup "Tolerant↔Strict" slider** |
| beam×10 | — | −2632 … −1192 | successor-entry viability gate |
| beam×100 | — | −2272 … −832 | word-exit propagation gate |
| competitor threshold | 0.01 | −720 | "still a live competitor" window (counts rivals; feeds the early-exit test) |
| state floor | — | $E000 = −8192 = −52.4 nat | "empty" state value; see §7.3 |

### 7.2 The scorer's observation formula

Per frame, for each active model instance (model index m, states
s = 0, 1, 2):

```
obs[s] = K − Σ (i = 0..5)  Bᵢ[ cwᵢ·1680 + m·3 + s ]
```

* `cwᵢ` — the six codeword bytes of the frame, in delivery order
  (acode, bcode, adcode, bdcode, xcode, p3code); the identity of
  buffer order, table order and training order is verified.
* `Bᵢ` — the six 53,760-byte observation tables (§6.4);
  1680 = 560 models × 3 states.
* **K = −(previous frame's best score)** — the per-frame normalizer:
  by construction the best path hovers near zero, and every other
  path's score *is* its deficit versus the best, in units. This is
  what makes int16 scores sufficient — and it has a consequence:
  "how far behind is this path" is always relative to *the best model
  of this particular frame*, so a voice that fits everything poorly
  still anchors the scale at its own best fit.

The six table addends are precomputed per frame (`Bᵢ + cwᵢ·1680` once,
then three adjacent bytes per model), and the three `obs[s]` values
are **cached per model** (560 × 3 int16, zeroed each frame), so many
instances of the same triphone score once. Every byte is
zero-extended; there is no rounding anywhere — the entire acoustic
likelihood computation is byte loads and 16-bit adds.

### 7.3 The Viterbi time update

The same routine advances the dynamic programming. Each active
instance keeps **two score planes** (current/previous, 3 states +
best-of + traceback tags), ping-ponged on the frame counter's low
bit. With t = the instance's model's 9-byte transition row (§6.5):

```
enter states carry obs[s] + incoming score − transition cost:
  state0' = max(state0 + obs[0] − t.self0, incoming-entry)
  state1' = max(state1 + obs[1] − t.self1, state0 + obs[0] − t.next0)
  state2' = max(state2 + obs[2] − t.self2, state1 + obs[1] − t.next1)
  exit    = state2 + obs[2] − t.exit2      → offered to the successor
                                              (next phone, or the word end)
```

with these engine-specific mechanics:

* **Fresh instances are floored at $E000 (−52.4 nat)**, and plane
  slots update only when the new score is **greater**. Combined with
  the per-frame normalization, a hypothesis that has fallen more than
  ~52 nats behind the frame best can never re-enter a word — the
  floor, not the beam, is the deep cliff at the bottom of the search.
* **Beam marking**: instances whose best state stays above
  `frameBest + beam` are marked alive for the next frame; the rest
  drop off the active list.
* **Word exits** that clear `frameBest + beam×100` are propagated into
  the grammar network (the word-end callback records a traceback
  token and seeds the successor arcs); successor phone entries are
  gated at `beam×10`.
* The frame's best score and best node are tracked as it goes;
  afterwards the frame-best cell holds the value whose negation
  becomes the next frame's K.

Two verified engine defects live here, both voice-symmetric (they tax
every speaker equally, documented for completeness): the **0→2 skip
transition is dead code** (its contribution is unconditionally
overwritten by the 1→2 write, so every phone must spend ≥ 3 frames
even though the tables train real skip costs), and a **newly activated
successor's entry score is floored in the same frame it was written**
(phone entry effectively delayed one frame, its first entry score
lost). And one operational hazard: if the active-instance list is
*empty* when a frame arrives — total beam starvation — the scorer
returns error $CE6E, and the frame loop abandons the utterance with no
result (one of the silent outcomes; §7.5).

### 7.4 The grammar network

At Start, the engine expands the current language model (§8.1) into a
network of **state records** (score, traceback tag; initialized at
$8300 = −32000) and **arc records**, with designated initial and
final states. Arc types select
their content when first expanded:

| arc type | resolves to |
|---|---|
| word | that word's phone-model chain (from the pronunciation machinery, §6.8) |
| silence | the silence model (index 51) |
| noise | the non-vocabulary models (6..11) |
| OOV (two flavors) | the out-of-vocabulary models (0..5) — the "someone said something else" path, carrying the language model's OOV penalty |
| ε (null) | recursive expansion (with a stack-space guard) |

Arcs carry additive penalties (per-word transition penalty, OOV
penalty) from language-model properties — the "language model score"
of this system is these constants; there are no n-gram probabilities
in a command grammar.

### 7.5 The frame loop

Pseudocode of the engine's per-utterance loop (entered with a grammar
set and the utterance object as frame source):

```
loop:
  status = frameSource.getFrame(buffer)      ; the 6 codeword bytes
  if status == NOTHING_YET:  exit CE65       ; no frame ready / timeout
  if status == ENDPOINTED:   exit reason 1   ; the in-band end marker
  if status == 0:            haveFrame = true

  if haveFrame:
     err = scorer.ScoreFrame(self)           ; §7.2 + §7.3
     if err: exit err                        ; e.g. CE6E starvation
     prune & expand: walk nodes touched this frame,
        propagate arcs (beam test at frameBest + beam),
        count live competitors (within −720 of the running best)
     optional early exits:
        eeal flag && exactly one survivor && it is the final state
                                        → exit CE67 ("collapsed")
        after ≥ meaf (50) frames, best node within meas (20 units)
        of frame best but with an empty traceback → exit CE66
  frame++ ; swap score planes
  if frame ≥ mnfe (3000):   exit CE6C        ; 30 s cap
```

Exit dispositions (the complete truth table — this is where silence
is decided):

| exit | meaning | words delivered? |
|---|---|---|
| reason 1 | front end endpointed the utterance | **yes** — build the result |
| CE67 | grammar's final node is the single survivor | **yes** |
| CE65 | no frame ready / wall-clock timeout | no — result builder not even called |
| CE66 | best-node traceback empty at in-search endpoint | no (empty result) |
| CE6C | frame cap | no (empty result) |
| any scorer/pruner error (CE6E, memory, stack) | abandoned | no (empty result) |

### 7.6 Building the result — and the three outcomes

On a delivering exit, the engine walks the **traceback**: the chain of
word-boundary tokens the winning path deposited (stored in a
hash-table word graph, capacity 300–2400 entries). Each traceback word
is then classified **by its model index** against the special ranges of
§6.2:

| surviving word's model index | becomes |
|---|---|
| a real vocabulary word | itself — recognized text |
| OOV range (0..5) | **the rejection word** — a special word object whose spelling is `???` (adjacent duplicates collapsed) |
| silence (51) | dropped |
| noise (6..11) | dropped |

The filtered list is the recognition result; its TEXT property (the
`rslt` component concatenates each word's spelling) is what feedback
displays. Now the system's three observable outcomes follow
mechanically:

* **Recognized command** — delivering exit, traceback contains real
  vocabulary words → the rules layer runs the action (§8.4).
* **"Pardon me?"** — delivering exit, but the surviving path went
  through OOV arcs: the result contains the `???` rejection word, and
  the "acknowledge failure of understanding" speech rule responds.
* **Nothing at all** — every other case: a non-delivering exit, an
  empty traceback, or a traceback whose every word was silence/noise
  (all dropped ⇒ empty word list). An empty result triggers no rule.
  This is *by design* — background chatter that matches nothing must
  not make the machine chirp — and it is also why several genuine
  failure modes (beam starvation, the ptfe drop paths of §5.8) are
  indistinguishable from "it ignored you" at the UI.

### 7.7 The confidence score

For delivering exits (utterances > 2 frames, when the client enabled
it), the engine attaches `srsc`:

```
S = final path score + Σ per-frame best scores      ; un-normalize K
S −= kept·nmtp + silences·sltp + noise·nvtp         ; word-count penalties
S −= 40 units × #streams × #frames                  ; the "matched" allowance
srsc = exp( (S in nats) / #frames )                 ; ∈ (0, 1]
```

i.e. **e^(mean per-frame log-likelihood ratio versus a well-matched
frame)** — 1.0 means "as good as the models ever expect"; the 40-unit
allowance is exactly the measured expected per-stream cost of matched
speech (§6.4). Clients can gate their own actions on it.

### 7.8 A worked utterance — "Computer, open the Trash"

Timeline for a clean, in-contract utterance (t in frames after the
attention word's first frame):

```
t≈−40…0   silence; EndPoint's mean has converged; scores sit at −1.6
t=0       speech onset; 7 positive frames later BeginEnd[0]=1 —
          the DSP starts streaming codewords; the engine opens an
          utterance and seeds the grammar's initial state
t=0…55    "Computer": the name-word arc's phone chain fits; competing
          first-words fall behind by tens of units/frame and drop out
          at frameBest−beam; word exit clears beam×100, deposits a
          traceback token, seeds the command arcs
t≈55…80   the short pause: silence-model arcs absorb it cheaply
t≈80…170  "open the Trash": three word arcs in sequence; rival
          commands ("open the Introduction…") die mid-word as their
          phones mismatch
t≈170     speech ends; EndPoint counts 40 negative frames
t=210     BeginEnd[1]=1; EndPad appends 30 padding frames and the
          negative end marker
t≈213     the drain delivers the marker; the frame loop exits reason 1;
          traceback = [sil] computer open the trash [sil] → classifier
          drops the silences → 4 words; srsc attached; the rules layer
          matches the Speakable Item and opens the Trash window
```

The failure modes trace the same timeline: speak the pause too long
(≥ 400 ms) and the utterance closes after "Computer" — two
utterances, neither parses. Feed a voice/channel the models never saw
and every word arc pays a per-frame tax on all six streams; when the
deficit reaches the beam the word paths die; if only silence/noise/OOV
paths survive you get "Pardon me?" — and if even those starve, or the
parse is silence end to end, you get nothing at all.

## 8. Grammar, rules, actions, feedback

The last link: where the vocabulary comes from, how listening is
gated, what happens when a phrase is recognized, and what the user
sees. This layer is what made Casper feel like a *product* rather than
a lab demo — and it is also where the system's famous silences are
decided.

### 8.1 Speech rules and dynamic language models

Casper does not have a fixed command list. The active vocabulary is
generated, continuously, from **speech rules** — a rule has five
parts: a name, flags, a **phrase list**, a **context expression**
(when is this rule active?), and an **action expression** (what does it
mean?). The rules compile into a finite-state **language model** —
the grammar network of §7.4 — and because rules carry contexts, only
phrases that make sense *right now* are in the network at any moment.
Keeping the active set small (a few dozen phrases) is the whole trick
of 1993-era usable accuracy: fewer competitors = wider effective
margins in the beam search.

The shipped rule sources:

* **System Speech Rules** (extension): the global grammar — the
  enumeration of the Speakable Items folder, menu commands ("open",
  window commands, file sharing), and the meta-rules, including
  **"acknowledge failure of understanding"** — the rule whose response
  is *"Pardon me?"*, triggered by a result containing the rejection
  word (§7.6).
* **My Speech Macros**: user-editable AppleScript rules ("what time is
  it", "what day is it", …) whose actions run scripts and typically
  speak their answer via text-to-speech.
* **Speakable Items** (`Apple Menu Items/Speakable Items/`): a folder
  of zero-byte **alias files whose names are the phrases**. One
  generic rule enumerates the folder; saying a file's name resolves
  the alias and opens the target. Adding a command = renaming a file.
  Pronunciations for arbitrary names come from the letter-to-sound
  machinery (§6.8).

The engine-side components: `bldr`:10 assembles language-model objects
(models contain phrases contain words, each word carrying its
spelling and properties — including the `rjct` flag that marks the
rejection word); `dict`:18 + `look`:17/18/19 supply pronunciations;
`srch`:20 consumes one compiled network per Start.

### 8.2 How the computer's name gates listening

Nothing in the engine knows the word "Computer" is special. The
gating is entirely grammatical: while idle, the active language model
requires the **name word at the front of every path** (`<name>
<command>` — plus the OOV/noise/silence arcs that make the network
robust). Background speech that does not fit the name-fronted paths
overwhelmingly parses through the silence/noise arcs — whose words
the result classifier **drops entirely** (§7.6), yielding an empty
result and therefore no reaction; live tracing confirms this
silence-only parse as the standard fate of non-matching speech
(*observed*). Only a parse that survives *through OOV word arcs*
produces the rejection word and hence "Pardon me?" — *observed* in
practice for utterances that got partway (e.g. name matched, command
not: the windoid shows "Computer ?"). The net effect is the shipped
behavior: the Mac sits in a meeting all day and never says a word
until someone addresses it by name. Once the name is matched,
follow-on grammars (the command set, and rule contexts referencing
the current application) become active.

The alternative gate is the **attention key** (default: Clear on the
keypad) — a *toggle*, not push-to-talk. Toggled off ("standing by"),
the system stops listening at the source (input DMA halts; the windoid
ear dims). There is also the automatic interlock of §4.14: while the
Mac itself produces audio (beeps, TTS replies), the endpointer is
suppressed, so the system never converses with itself.

### 8.3 The Speech Setup control panel, mapped to mechanisms

| control | mechanism it drives |
|---|---|
| Recognition On/Off | launches/quits the whole stack (recognizer processes, DSP task; ~10–15 s startup while models load and the DSP graph is built) |
| **Tolerant ↔ Strict** slider | the engine's `sped` property (0..100) → the **beam width**, −2992 … −1552 units (§7.1): Tolerant = wider beam = more survivors = higher hit *and* false-alarm rate; Strict = the reverse |
| Name | the `<name>` word of the gating grammar (respelled via letter-to-sound) |
| Attention key | the listening toggle (§8.2) |
| Feedback options | SR Monitor's character/voice behavior |

### 8.4 From result to action

A delivering search exit hands the client an ordered word list
(§7.6). The rules layer:

1. Reads the result's text/word objects (`rslt`:30's TEXT property
   concatenates the spellings — literally including `???` when the
   rejection word survived).
2. Matches it back to the generating rule and evaluates that rule's
   **action**: resolve-and-open a Speakable Item alias, drive a menu
   command, or run an AppleScript (whose reply is often spoken —
   which suppresses the endpointer while it plays, then listening
   resumes).
3. Drives feedback (§8.5).

Recognition-confidence (`srsc`, §7.7) is available to this layer as an
additional gate on acting vs. ignoring.

### 8.5 SR Monitor — what the user sees

The feedback windoid (bottom-left; an 83-frame animated character) is
a separate small application fed by the recognizer's client side:

| display | state it reflects |
|---|---|
| ear icon, dark | listening ("Ready.") |
| ear icon, dimmed | standing by (attention key toggled off) |
| "Starting up… / Ready." | the startup sequence |
| animated "sound waves" | an utterance is currently open (driven by the front end's level while BeginEnd[0] is up) |
| a text line, e.g. **"Computer open the Trash"** | the recognized phrase (the result's TEXT) |
| **"Computer ?"** + spoken *"Pardon me?"* | a delivered result whose content was the rejection word — heard, not understood |
| nothing changes | everything else (see below) |

### 8.6 The three outcomes, end to end

Assembling §4–§7 into the complete decision tree for one spoken
phrase:

```
                     did EndPoint open an utterance?
                       │no (too quiet / suppressed /
                       │    standing by)                → NOTHING
                       ▼yes
                did the codeword stream reach the engine?
                       │no (any ptfe drop path, §5.8)   → NOTHING
                       ▼yes
                did the frame loop exit delivering?
                       │no (starved beam, timeout, cap)  → NOTHING
                       ▼yes
                what survived in the traceback?
                       │only silence/noise (all dropped) → NOTHING
                       │OOV words → "???"               → "Pardon me?"
                       ▼real vocabulary words
                the rule's action runs; text displayed   → RECOGNIZED
```

The essential asymmetry: **the system only ever speaks up when a
delivered utterance parsed through actual model paths.** Every earlier
failure — level, endpointing, transport, pruning — collapses into the
same user experience: the ear blinked, and nothing happened. That is
simultaneously the design (a polite always-on listener) and the
debugging challenge (a dozen distinct failures share one symptom;
Appendix A.1 points to the instrumentation that distinguishes them).

### 8.7 Timing budget, microphone to screen

| segment | time |
|---|---|
| A/D + DMA + kernel convert | < 1 frame |
| front end (per frame) | well under the 10 ms frame budget (the whole chain is ~2500 instructions/frame + VQ search) |
| feature lookahead (VecGen6) | 30 ms |
| utterance close detection | 400 ms of trailing silence |
| tail padding | 300 ms |
| search | overlapped with speech (frame-synchronous); the finalize is milliseconds |
| rules + action + redraw | UI-bound; tens to hundreds of ms |

So the felt latency from *finishing the phrase* to *seeing the
response* is dominated by the fixed 0.7 s of close-detection plus
padding — a deliberate trade for not truncating slow speakers.

## 9. The engine's error codes

The srch/loop error domain, complete — including the silent
abandonments that never reach any UI (§5.8, §7.5). The framing,
front-end and engine constants, and the shipped binary formats, are
stated where each mechanism is described (§4, §5.3, §6.6, §7.1).

The table (codes are the components' own error domain):

| code | meaning |
|---|---|
| $CD46 | property read-only |
| $CD48 | buffer too small |
| $CD4B | NULL/bad parameter |
| $CD51 | sped out of range (>100) |
| $CD55 | stack space < 8 KB during arc recursion |
| $CE01 | NULL model argument |
| $CE65 | no frame ready / timed out (result builder not called) |
| $CE66 | empty traceback at in-search endpoint |
| $CE67 | final node dominates (delivering exit) |
| $CE68 / $CE69 | not initialised / not started |
| $CE6A / $CE6B | NULL lexicon / no language model |
| $CE6C | frame limit (3000) reached |
| $CE6D | scorer rejected the model |
| $CE6E | **scorer: no active instances (beam starvation)** — silent abandonment |
| $CE6F | model topology tag not accepted |
| $CE70 | stream-count mismatch (frame source streams ≠ #cbk) |
| $CE71 / $CE74 / $CE76 | hash alloc failed / traceback full / pool exhausted |
| $CE73 | can't set property while running |
| $CE75 | no search network |
| $CE78 | unknown arc type |
| $CE79 | model has no OOV range |

## Appendix A. Verification status and sensitivity

### A.1 Verification status, summarized

The DSP modules WinAuto, LpcCep, FreqWp, EnvNorm, EndPoint, VecGen6,
VQ, UttRjct and the IEEE/DSP32 converters were **executed against
independent reference implementations** (bit-level or ≤ 1.6e-3
agreement); the scorer's tables and formula were **byte-verified
against the shipped data** with a worked numeric example; ptfe,
EndPad, and the srch frame-loop mechanics are **instruction-traced
decodes** cross-confirmed by live logpoint runs — frame-loop exits,
harvest counts, delivered outcomes. The status stated under each
module in §4 is authoritative per module. Everything not marked
*observed*, *inferred — unverified*, or "verified" traces to the
shipped software's disassembly [7] or to Apple's published
documentation [1] [5].

### A.2 The sensitive variable: the input channel

The arithmetic of the whole chain is verified faithful. The
practically sensitive variable is the **input channel**: the models
were trained through the PlainTalk microphone and the Singer analog
path, and measurement places the acceptance region only +2..4 dB of
spectral tilt from collapse on the bright side (≥ 12 dB tolerant on
the dark side) [7]. Faithful channel modeling — microphone
coloration, in-contract levels, no ≥ 400 ms internal pauses — matters
more than further numeric fidelity. This is why §2.1 insists the
100–200 mVpp contract is a *system* contract: it is the level window
in which those margins were measured.

## References

1. Apple Computer, Inc., *Developer Note: Macintosh Quadra 840AV and
   Macintosh Centris 660AV Computers*, Developer Press, 1993 — the AV
   hardware: the DSP, the PSC and its DMA channels, Singer, sound I/O
   and the speech-recognition microphone (Chapter 2); the Real Time
   Manager and frame organization (Chapter 3); "How Does Casper
   Work?" (p. 318).
2. AT&T Microelectronics, *AT&T DSP3210 Digital Signal Processor
   Information Manual* (document MN91-006OMOS, September 1991) — the
   DSP3210 architecture, the DAU and its pipeline latencies (§8.2.6),
   the floating-point format (§3.3.3), and the VCOS operating
   environment the Real Time Architecture builds on.
3. AT&T Microelectronics, *AT&T DSP3210 Digital Signal Processor —
   The Multimedia Solution*, Data Sheet, March 1993 — packaging,
   electrical and timing specifications of the 66.67 MHz part.
4. Kai-Fu Lee, *Automatic Speech Recognition: The Development of the
   SPHINX Recognition System*, Kluwer Academic Publishers, 1989 — the
   large-vocabulary, speaker-independent, continuous-speech recognizer
   whose discrete multi-stream HMM recipe Casper's front end and
   acoustic models follow.
5. Apple Computer, Inc., *Inside Macintosh: Speech Recognition*,
   Addison-Wesley, 1997 — the Speech Recognition Manager's published
   programming interface: speech objects, recognizers, language models,
   and the recognition loop as Apple documented them for PlainTalk-era
   (and later) recognizers.
6. Lawrence R. Rabiner, "A Tutorial on Hidden Markov Models and
   Selected Applications in Speech Recognition," *Proceedings of the
   IEEE*, vol. 77, no. 2, February 1989 — the discrete-HMM formalism
   and the Viterbi search as of the design era.
7. Apple Computer, Inc., PlainTalk speech software for the Quadra
   840AV / Centris 660AV (SR North American English, PlainTalk Speech
   Recognition, SR Monitor, System Speech Rules, 1993) — disassembly
   and decode of the 26 `dspf` DSP modules (the 12 live in this chain
   verified against independent reference implementations), the
   `ptfe` host driver, and the `srch`/`loop`/`bldr`/`rslt`/`dict`/
   `look`/`hmms` components; live frame-by-frame tracing of accepted
   and rejected utterances; the acceptance-region measurements of
   Appendix A.2.
8. Alan V. Oppenheim and D. H. Johnson, "Discrete Representation of
   Signals," *Proceedings of the IEEE*, vol. 60, no. 6, June 1972 —
   the all-pass frequency-warping operator Casper's FreqWp module
   applies directly to the cepstrum.
