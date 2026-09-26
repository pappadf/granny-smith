// The audio-out ring's consumer: everything the AudioWorklet does per render
// quantum, as a plain class over a SharedArrayBuffer so it is unit-tested
// (tests/unit/audioRing.test.ts). The worklet (gsAudio.worklet.ts) is a thin
// shell around it. The producer is em_audio.c's platform_audio_push.
//
// Real-time machinery on the consumer side (per the sound-emulation
// strategy): linear-interpolation resampling with PI rate trim (±2000 ppm), a
// start gate at the target depth, silence-aware depth trimming, per-quantum
// volume ramping, a one-pole LPF and a DC blocker. The producer stays
// deterministic.
//
// One writer per index (T4, F-35): the emulator writes WRITE, this class
// writes READ, both free-running uint32 in Int32 words. A full ring is ours
// to notice -- the producer overwrites the oldest frames, and we resync when
// more than a ring's worth is outstanding. A new stream is requested through
// RESET_GEN and carried out here. Only the worklet whose id is in OWNER
// consumes; a retired one stops. READ used to have five writers.

import {
  ARING_MAGIC,
  ARING_VERSION,
  ARING_W_DATA_OFF,
  ARING_W_FILL_PM,
  ARING_W_FRAMES,
  ARING_W_OWNER,
  ARING_W_PUSH_SEQ,
  ARING_W_READ,
  ARING_W_RESET_GEN,
  ARING_W_SILENT,
  ARING_W_VOL,
  ARING_W_WRITE,
  shmBlockOk,
} from '@/bus/shmLayout';

export interface AudioRingOptions {
  srcRate: number;
  dstRate: number;
  channels: number;
  targetLatency: number; // seconds
  owner: number;
}

export class AudioRingConsumer {
  private hdr: Int32Array;
  private data: Int16Array;
  private readonly mask: number;
  private readonly capacity: number;
  private readonly ch: number;
  private readonly owner: number;
  private readonly targetLatency: number;
  private readonly dstRate: number;
  private srcRate: number;
  private step: number;
  private targetFrames: number;
  private errI = 0;
  private frac = 0;
  private readonly lpfY = [0, 0];
  private readonly lpfA: number;
  // DC blocker (speaker AC coupling): the ASC DAC output carries a large
  // constant offset (offset-binary DAC, unused wavetable voices at rail), and
  // the underrun/start-gate fill value is 0 -- without DC removal every
  // delivery hiccup steps between the offset and 0, an audible full-scale
  // click. fc ~20 Hz: inaudible, settles in a few ms.
  private readonly dcX = [0, 0];
  private readonly dcYv = [0, 0];
  private readonly dcR: number;
  private quietQ = 0;
  private lastSeq = 0;
  private statQ = 0;
  private curGain = 0;
  private started = false;
  private gen: number;
  private flushPending = false;
  private dead = false;
  underruns = 0;

  // Throws when the block is not a layout this build speaks.
  constructor(sab: SharedArrayBuffer, ringPtr: number, o: AudioRingOptions) {
    const i32 = new Int32Array(sab);
    if (!shmBlockOk(i32, ringPtr, ARING_MAGIC, ARING_VERSION))
      throw new Error('audio ring layout not recognised');
    this.hdr = new Int32Array(sab, ringPtr, 16);
    this.capacity = Atomics.load(this.hdr, ARING_W_FRAMES);
    this.mask = this.capacity - 1;
    this.ch = o.channels || 1;
    this.data = new Int16Array(
      sab,
      ringPtr + Atomics.load(this.hdr, ARING_W_DATA_OFF),
      this.capacity * this.ch,
    );
    this.owner = o.owner;
    this.targetLatency = o.targetLatency || 0.083;
    this.dstRate = o.dstRate;
    this.srcRate = o.srcRate || 22257;
    this.step = this.srcRate / this.dstRate;
    this.targetFrames = Math.floor(this.targetLatency * this.srcRate);
    this.lpfA = 1.0 - Math.exp((-2 * Math.PI * 8000) / this.dstRate);
    this.dcR = 1 - (2 * Math.PI * 20) / this.dstRate;
    // A request made before we existed is already honoured: start at WRITE.
    this.gen = Atomics.load(this.hdr, ARING_W_RESET_GEN);
    this.flushPending = true;
  }

  // A rate change is a stream restart: flush, re-gate, ramp.
  setRate(rate: number): void {
    this.srcRate = rate;
    this.step = this.srcRate / this.dstRate;
    this.targetFrames = Math.floor(this.targetLatency * this.srcRate);
    this.flushPending = true;
  }

  // Stop touching the shared ring (the page replaced this worklet).
  retire(): void {
    this.dead = true;
  }

  private restartGate(): void {
    this.frac = 0;
    this.errI = 0;
    this.started = false;
    this.curGain = 0;
  }

  // Fill one render quantum. False: retired, stop processing.
  process(out: Float32Array[]): boolean {
    if (this.dead) return false;
    const hdr = this.hdr;
    if (Atomics.load(hdr, ARING_W_OWNER) !== this.owner) {
      this.dead = true; // a newer worklet owns the ring
      return false;
    }
    const frames = out[0].length;
    const ch = this.ch;
    const oc = out.length < ch ? out.length : ch;
    const mask = this.mask;
    const w = Atomics.load(hdr, ARING_W_WRITE) >>> 0;
    let r = Atomics.load(hdr, ARING_W_READ) >>> 0;

    // A new stream (requested by the producer, or a rate change): drop what
    // is queued and re-gate.
    const gen = Atomics.load(hdr, ARING_W_RESET_GEN);
    if (gen !== this.gen || this.flushPending) {
      this.gen = gen;
      this.flushPending = false;
      r = w;
      this.restartGate();
    }
    let avail = (w - r) >>> 0;
    // The producer lapped us (it overwrites a full ring): the oldest frames
    // are gone, so resync to the target depth behind the newest.
    if (avail > this.capacity) {
      r = (w - Math.min(this.targetFrames, this.capacity)) >>> 0;
      avail = (w - r) >>> 0;
    }

    // Quiet-stream detector: quanta since the producer's push_seq moved.
    const seq = Atomics.load(hdr, ARING_W_PUSH_SEQ);
    if (seq !== this.lastSeq) {
      this.lastSeq = seq;
      this.quietQ = 0;
    } else {
      this.quietQ++;
    }
    // Fill-level report every 32 quanta (~85 ms at 48 kHz), read by the
    // emulator's adaptive governor as back-pressure (a draining ring = the
    // real-time deadline being missed).
    if ((++this.statQ & 31) === 0) {
      const pm = Math.round((avail / (this.targetFrames || 1)) * 1000);
      Atomics.store(hdr, ARING_W_FILL_PM, Math.max(0, Math.min(2000, pm)));
    }
    // Start gate: stay silent until the ring reaches the target depth, OR
    // until the stream has evidently ended short of it (data pending but no
    // push for 12 quanta ~32 ms) -- so sounds shorter than the cushion still
    // play out. An active producer pushes every ~3 ms, so the quiet threshold
    // must sit above scheduling jank (tens of ms) or the gate opens mid-fill
    // with a shallow ring and the crackle returns.
    if (!this.started) {
      const ready = avail >= this.targetFrames || (avail > 0 && this.quietQ >= 12);
      if (!ready) {
        for (let c = 0; c < out.length; c++) out[c].fill(0);
        Atomics.store(hdr, ARING_W_READ, r | 0);
        return true;
      }
      this.started = true;
    }
    const ts = this.targetFrames;
    const targetGain = Math.min(Math.max(Atomics.load(hdr, ARING_W_VOL), 0), 7) / 7;
    // PI controller trims the resample step ±2000 ppm toward target depth.
    const error = avail - ts;
    this.errI = this.errI * 0.995 + error * 0.005;
    let adj = (error / ts) * 0.005 + (this.errI / ts) * 0.001;
    if (adj > 0.002) adj = 0.002;
    else if (adj < -0.002) adj = -0.002;
    const stepAdj = this.step * (1 + adj);
    const gainStep = (targetGain - this.curGain) / frames;
    for (let i = 0; i < frames; i++) {
      const have = (w - r) >>> 0;
      if (have < 2) {
        this.underruns++;
        this.curGain += gainStep;
        for (let c = 0; c < oc; c++) out[c][i] = 0;
        continue;
      }
      const i0 = this.frac | 0;
      let i1 = i0 + 1;
      if (i1 >= have) i1 = have - 1;
      const b0 = ((r + i0) & mask) * ch;
      const b1 = ((r + i1) & mask) * ch;
      const frac = this.frac - i0;
      this.curGain += gainStep;
      for (let c = 0; c < oc; c++) {
        const s0 = this.data[b0 + c] / 32768;
        const s1 = this.data[b1 + c] / 32768;
        const x = s0 + (s1 - s0) * frac; // linear interpolation
        this.lpfY[c] += this.lpfA * (x - this.lpfY[c]); // one-pole LPF
        const lp = this.lpfY[c];
        const hp = lp - this.dcX[c] + this.dcR * this.dcYv[c]; // DC blocker
        this.dcX[c] = lp;
        this.dcYv[c] = hp;
        out[c][i] = hp * this.curGain;
      }
      this.frac += stepAdj;
      const consumed = this.frac | 0;
      if (consumed > 0) {
        this.frac -= consumed;
        r = (r + (consumed < have ? consumed : have)) >>> 0;
      }
    }
    // Re-arm the start gate when the stream runs dry (a sound ended, or a
    // deep underrun): without this only the FIRST sound after page load gets
    // the target-depth cushion. The <2 remnant is discarded (r = w): a nonzero
    // depth would let the quiet-stream early-open defeat the gate for the
    // next sound.
    const w2 = Atomics.load(hdr, ARING_W_WRITE) >>> 0;
    if (this.started && (w2 - r) >>> 0 < 2) {
      this.started = false;
      r = w2;
      this.frac = 0;
      this.errI = 0;
    }
    // Silence-aware depth trim: during sustained silence, clamp the depth to
    // the target so latency can't grow when the emulator outruns real time.
    if (Atomics.load(hdr, ARING_W_SILENT) >= 8) {
      const depth = (w2 - r) >>> 0;
      if (depth > ts) r = (r + depth - ts) >>> 0;
    }
    Atomics.store(hdr, ARING_W_READ, r | 0); // our index, our store
    return true;
  }
}
