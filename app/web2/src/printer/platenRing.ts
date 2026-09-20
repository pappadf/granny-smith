// The platen worker's ring loop: consumes the printer bridge's records
// from the outbound ring in the emulator's shared heap, runs them against
// the platen library, and answers through the inbound ring
// (platenProtocol.ts / laserwriter_ring_protocol.h).  Factored out of the
// worker so a vitest can drive it against a plain SharedArrayBuffer with
// no Worker at all: it takes the shared memory, the control block's
// address, the library, and a host that receives the finished PDF.
//
// Threading: the bridge publishes bytes by advancing OUT_HEAD and
// notifying; the loop parks in Atomics.waitAsync on that word, consumes
// every complete record up to it, and advances OUT_TAIL as it goes.  Its
// answers advance IN_HEAD (notified, though the core only polls); when
// the inbound ring is full it waits on IN_TAIL for the core to consume.
// A framing violation, a library exception or a module failure marks the
// worker LOST, after which the core fails whatever it was waiting on.

import {
  ABANDON_JOB,
  C_IN_HEAD,
  C_IN_OFF,
  C_IN_SIZE,
  C_IN_TAIL,
  C_MAGIC,
  C_OUT_HEAD,
  C_OUT_OFF,
  C_OUT_SIZE,
  C_OUT_TAIL,
  C_STAT_FEEDS,
  C_STAT_JOBS,
  C_STATUS,
  C_VERSION,
  CTRL_WORDS,
  FEED_ENDED,
  FEED_FAILED,
  FEED_JOB,
  FEED_LEN,
  FEED_SEQ,
  FEED_WAITING,
  FEED_WORDS,
  FED_F_TRUNCATED,
  FINISH_JOB,
  FINISH_TITLE_LEN,
  FINISH_WORDS,
  FINISHED_WORDS,
  getU32,
  MAGIC,
  OPEN_BUDGET_H,
  OPEN_BUDGET_L,
  OPEN_COMPRESS,
  OPEN_EMBED,
  OPEN_ID_BYTES,
  OPEN_ID_COUNT,
  OPEN_JOB,
  OPEN_PASSWORD,
  OPEN_PRELUDE,
  OPEN_WORDS,
  OUTCOME_BUDGET,
  OUTCOME_ERROR,
  OUTCOME_FAILED,
  OUTCOME_OK,
  PROTOCOL_VERSION,
  R_ABANDON,
  R_FED,
  R_FEED,
  R_FINISH,
  R_FINISHED,
  R_OPEN,
  R_OPEN_FAILED,
  R_OPENED,
  R_PAD,
  ringRead,
  ringWrite,
  STATUS_ATTACHED,
  STATUS_LOST,
  TEXT_MAX,
  TITLE_MAX,
  type Ring,
} from './platenProtocol';
import {
  PLATEN_DONE,
  PLATEN_OK,
  PLATEN_OUTCOME_BUDGET,
  PLATEN_OUTCOME_ERROR,
  PLATEN_OUTCOME_OK,
  type PlatenEntry,
  type PlatenLib,
} from './platenLib';

// A finished document, posted to the page (the PDF as a transferable).
export interface DocumentMsg {
  type: 'document';
  jobId: number;
  title: string;
  name: string; // the download's file name
  pages: number;
  pdf: Uint8Array;
}

// Where the loop hands a document: the worker's postMessage, or a test's
// recorder.
export interface RingHost {
  post(msg: DocumentMsg, transfer: Transferable[]): void;
}

const EMPTY = new Uint8Array(0);
const encoder = new TextEncoder();
const decoder = new TextDecoder();

// The download's name: <job id, 5 digits>-<title>.pdf, the title made
// file-safe the way headless_main.c names its print-dir files (one '_'
// per run of anything outside [A-Za-z0-9.-], leading runs dropped, at
// most TITLE_MAX characters; "untitled" when nothing is left).
export function documentName(jobId: number, title: string): string {
  let safe = '';
  let pendingSep = false;
  for (const ch of title) {
    if (safe.length >= TITLE_MAX) break;
    if (/^[A-Za-z0-9.-]$/.test(ch)) {
      if (pendingSep && safe.length > 0 && safe.length < TITLE_MAX) safe += '_';
      pendingSep = false;
      if (safe.length < TITLE_MAX) safe += ch;
    } else {
      pendingSep = true;
    }
  }
  return `${String(jobId >>> 0).padStart(5, '0')}-${safe || 'untitled'}.pdf`;
}

// Marks the control block at `ctrlPtr` LOST (used when no PlatenRing
// could be built, e.g. a version mismatch) and wakes anyone parked on it.
export function markRingLost(buffer: SharedArrayBuffer | ArrayBuffer, ctrlPtr: number): void {
  const ctrl = new Int32Array(buffer, ctrlPtr, CTRL_WORDS);
  Atomics.store(ctrl, C_STATUS, STATUS_LOST);
  Atomics.notify(ctrl, C_STATUS);
  Atomics.notify(ctrl, C_IN_HEAD);
}

export class PlatenRing {
  private readonly ctrl: Int32Array;
  private readonly out: Ring; // core -> worker
  private readonly inb: Ring; // worker -> core
  private readonly lib: PlatenLib;
  private readonly host: RingHost;
  private consumed: number; // outbound bytes consumed (mod 2^32)
  private written: number; // inbound bytes written (mod 2^32)
  private job = 0; // the live platen job handle, 0 when none
  private jobId = 0; // its bridge job id
  private running = false;
  private lost = false;

  // Attaches to the control block at `ctrlPtr`: checks the magic and the
  // protocol version (throws on a mismatch, nothing touched), reads the
  // ring geometry, and publishes STATUS ATTACHED.  Records the bridge
  // wrote before the attach are still in the ring and are consumed by
  // the first service.
  constructor(
    memory: WebAssembly.Memory | SharedArrayBuffer,
    ctrlPtr: number,
    lib: PlatenLib,
    host: RingHost,
  ) {
    const buffer = memory instanceof SharedArrayBuffer ? memory : memory.buffer;
    const u8 = new Uint8Array(buffer);
    const ctrl = new Int32Array(buffer, ctrlPtr, CTRL_WORDS);
    const magic = Atomics.load(ctrl, C_MAGIC) >>> 0;
    const version = Atomics.load(ctrl, C_VERSION) >>> 0;
    if (magic !== MAGIC || version !== PROTOCOL_VERSION)
      throw new Error(
        `control block mismatch: magic ${magic.toString(16)} version ${version} (want ${PROTOCOL_VERSION})`,
      );
    this.ctrl = ctrl;
    this.out = { u8, base: ctrlPtr + ctrl[C_OUT_OFF], size: ctrl[C_OUT_SIZE] >>> 0 };
    this.inb = { u8, base: ctrlPtr + ctrl[C_IN_OFF], size: ctrl[C_IN_SIZE] >>> 0 };
    this.lib = lib;
    this.host = host;
    this.consumed = Atomics.load(ctrl, C_OUT_TAIL) >>> 0;
    this.written = Atomics.load(ctrl, C_IN_HEAD) >>> 0;
    Atomics.store(ctrl, C_STATUS, STATUS_ATTACHED);
    Atomics.notify(ctrl, C_STATUS);
  }

  // The worker gave up: the core fails its outstanding request.
  markLost(): void {
    this.lost = true;
    this.running = false;
    Atomics.store(this.ctrl, C_STATUS, STATUS_LOST);
    Atomics.notify(this.ctrl, C_STATUS);
    Atomics.notify(this.ctrl, C_IN_HEAD);
  }

  // Ends run() at its next turn.
  stop(): void {
    this.running = false;
    Atomics.notify(this.ctrl, C_OUT_HEAD);
    Atomics.notify(this.ctrl, C_IN_TAIL);
  }

  // Parks on OUT_HEAD and services every record published, until stop()
  // or a failure (which marks the worker LOST and rethrows).
  async run(): Promise<void> {
    if (this.running) return;
    this.running = true;
    try {
      while (this.running && !this.lost) {
        const head = Atomics.load(this.ctrl, C_OUT_HEAD) >>> 0;
        if (head === this.consumed) {
          const r = Atomics.waitAsync(this.ctrl, C_OUT_HEAD, head | 0, 1000);
          if (r.async) await r.value;
          continue;
        }
        await this.service();
      }
    } catch (e) {
      this.markLost();
      throw e;
    } finally {
      this.running = false;
    }
  }

  // Consumes every complete record published so far and answers each.
  // Returns true when anything was consumed.  Throws on a framing
  // violation or a library failure (the caller marks the worker LOST).
  async service(): Promise<boolean> {
    let any = false;
    for (;;) {
      const head = Atomics.load(this.ctrl, C_OUT_HEAD) >>> 0;
      if ((head - this.consumed) >>> 0 < 8) break;
      const rec = ringRead(this.out, this.consumed, head);
      const p = rec.payload;
      const u8 = this.out.u8;
      switch (rec.kind) {
        case R_PAD:
          break;
        case R_OPEN:
          await this.onOpen(u8, p, rec.len - 8);
          break;
        case R_FEED:
          await this.onFeed(u8, p, rec.len - 8);
          break;
        case R_FINISH:
          await this.onFinish(u8, p, rec.len - 8);
          break;
        case R_ABANDON:
          this.onAbandon(getU32(u8, p + 4 * ABANDON_JOB));
          break;
        default:
          throw new Error(`unknown ring record kind ${rec.kind}`);
      }
      this.consumed = (this.consumed + rec.len) >>> 0;
      Atomics.store(this.ctrl, C_OUT_TAIL, this.consumed | 0);
      any = true;
    }
    return any;
  }

  // --- requests -------------------------------------------------------------

  // OPEN: build the platen_config from the record and create the job.
  private async onOpen(u8: Uint8Array, p: number, payloadLen: number): Promise<void> {
    const jobId = getU32(u8, p + 4 * OPEN_JOB);
    const idCount = getU32(u8, p + 4 * OPEN_ID_COUNT);
    const idBytes = getU32(u8, p + 4 * OPEN_ID_BYTES);
    const preludeLen = getU32(u8, p + 4 * OPEN_PRELUDE);
    if (4 * OPEN_WORDS + idBytes + preludeLen > payloadLen)
      throw new Error(`malformed OPEN record for job ${jobId}`);
    // A job still alive here lost its ABANDON (no room): free it first
    if (this.job) this.freeJob();
    // The identity block: idCount NUL-terminated key/value pairs
    const identity: PlatenEntry[] = [];
    let q = p + 4 * OPEN_WORDS;
    const idEnd = q + idBytes;
    for (let i = 0; i < idCount; i++) {
      const key = readCString(u8, q, idEnd);
      q += key.bytes + 1;
      const value = readCString(u8, q, idEnd);
      q += value.bytes + 1;
      identity.push({ key: key.text, value: value.text });
    }
    const prelude = u8.slice(idEnd, idEnd + preludeLen);
    const job = this.lib.jobNew({
      identity,
      prelude,
      serverPassword: getU32(u8, p + 4 * OPEN_PASSWORD) | 0,
      compress: getU32(u8, p + 4 * OPEN_COMPRESS) !== 0,
      embedAllFonts: getU32(u8, p + 4 * OPEN_EMBED) !== 0,
      stepBudgetLo: getU32(u8, p + 4 * OPEN_BUDGET_L),
      stepBudgetHi: getU32(u8, p + 4 * OPEN_BUDGET_H),
    });
    if (!job) {
      const text = cap(encoder.encode(this.lib.lastError() || 'platen_job_new failed'), TEXT_MAX);
      await this.write(R_OPEN_FAILED, [jobId, text.length], [text]);
      return;
    }
    this.job = job;
    this.jobId = jobId;
    Atomics.add(this.ctrl, C_STAT_JOBS, 1);
    await this.write(R_OPENED, [jobId], []);
  }

  // FEED: run the bytes, drain both channels, answer FED.
  private async onFeed(u8: Uint8Array, p: number, payloadLen: number): Promise<void> {
    const jobId = getU32(u8, p + 4 * FEED_JOB);
    const seq = getU32(u8, p + 4 * FEED_SEQ);
    const len = getU32(u8, p + 4 * FEED_LEN);
    if (4 * FEED_WORDS + len > payloadLen)
      throw new Error(`malformed FEED record for job ${jobId}`);
    if (!this.job || jobId !== this.jobId) {
      await this.write(R_FED, [jobId, seq, FEED_FAILED, 0, 0, 0, 0], []);
      return;
    }
    const bytes = u8.slice(p + 4 * FEED_WORDS, p + 4 * FEED_WORDS + len);
    const r = this.lib.feed(this.job, bytes);
    Atomics.add(this.ctrl, C_STAT_FEEDS, 1);
    const status = r === PLATEN_OK ? FEED_WAITING : r === PLATEN_DONE ? FEED_ENDED : FEED_FAILED;
    const reply = this.lib.readReplies(this.job, TEXT_MAX);
    const errors = this.lib.readErrors(this.job, TEXT_MAX);
    const flags = reply.truncated || errors.truncated ? FED_F_TRUNCATED : 0;
    await this.write(
      R_FED,
      [
        jobId,
        seq,
        status,
        this.lib.pages(this.job),
        reply.bytes.length,
        errors.bytes.length,
        flags,
      ],
      [reply.bytes, errors.bytes],
    );
  }

  // FINISH: run to completion, post the PDF to the page, answer
  // FINISHED, free the job.
  private async onFinish(u8: Uint8Array, p: number, payloadLen: number): Promise<void> {
    const jobId = getU32(u8, p + 4 * FINISH_JOB);
    let titleLen = getU32(u8, p + 4 * FINISH_TITLE_LEN);
    if (4 * FINISH_WORDS + titleLen > payloadLen) titleLen = 0;
    const title = decoder.decode(u8.slice(p + 4 * FINISH_WORDS, p + 4 * FINISH_WORDS + titleLen));
    if (!this.job || jobId !== this.jobId) {
      const text = encoder.encode('no such job');
      await this.writeFinished(jobId, OUTCOME_FAILED, 0, text, EMPTY, EMPTY, EMPTY, 0);
      return;
    }
    const job = this.job;
    const r = this.lib.finish(job);
    const reply = this.lib.readReplies(job, TEXT_MAX);
    const errors = this.lib.readErrors(job, TEXT_MAX);
    const pages = this.lib.pages(job);
    let outcome: number;
    let errName: Uint8Array;
    let offending = EMPTY;
    if (r === PLATEN_OUTCOME_OK) {
      outcome = OUTCOME_OK;
      errName = EMPTY;
    } else if (r === PLATEN_OUTCOME_ERROR) {
      outcome = OUTCOME_ERROR;
      errName = encoder.encode(this.lib.errorName(job));
      offending = encoder.encode(this.lib.offending(job));
    } else if (r === PLATEN_OUTCOME_BUDGET) {
      outcome = OUTCOME_BUDGET;
      errName = EMPTY;
    } else {
      outcome = OUTCOME_FAILED;
      errName = encoder.encode(this.lib.lastError() || `platen_job_finish returned ${r}`);
    }
    // The document: a job that shows no page and ends cleanly is a query
    // or a placeholder (the bridge counts it the same way); an error keeps
    // what it rendered
    if (outcome !== OUTCOME_FAILED && (pages > 0 || outcome !== OUTCOME_OK)) {
      const pdf = this.lib.pdf(job);
      if (pdf.length)
        this.host.post(
          { type: 'document', jobId, title, name: documentName(jobId, title), pages, pdf },
          [pdf.buffer],
        );
    }
    this.freeJob();
    const flags = reply.truncated || errors.truncated ? FED_F_TRUNCATED : 0;
    await this.writeFinished(
      jobId,
      outcome,
      pages,
      cap(errName, TEXT_MAX),
      cap(offending, TEXT_MAX),
      reply.bytes,
      errors.bytes,
      flags,
    );
  }

  // ABANDON: the connection went away; free without finishing.
  private onAbandon(jobId: number): void {
    if (this.job && jobId === this.jobId) this.freeJob();
  }

  private freeJob(): void {
    if (this.job) this.lib.free(this.job);
    this.job = 0;
    this.jobId = 0;
  }

  // --- answers --------------------------------------------------------------

  private writeFinished(
    jobId: number,
    outcome: number,
    pages: number,
    errName: Uint8Array,
    offending: Uint8Array,
    reply: Uint8Array,
    errors: Uint8Array,
    flags: number,
  ): Promise<void> {
    const words = new Array<number>(FINISHED_WORDS);
    words[0] = jobId;
    words[1] = outcome;
    words[2] = pages;
    words[3] = errName.length;
    words[4] = offending.length;
    words[5] = reply.length;
    words[6] = errors.length;
    words[7] = flags;
    return this.write(R_FINISHED, words, [errName, offending, reply, errors]);
  }

  // Writes one inbound record, waiting on IN_TAIL while the ring has no
  // room, then publishes IN_HEAD.
  private async write(kind: number, words: number[], texts: Uint8Array[]): Promise<void> {
    for (;;) {
      const tail = Atomics.load(this.ctrl, C_IN_TAIL) >>> 0;
      const wr = ringWrite(this.inb, this.written, tail, kind, words, texts);
      if (wr >= 0) {
        this.written = wr;
        Atomics.store(this.ctrl, C_IN_HEAD, wr | 0);
        Atomics.notify(this.ctrl, C_IN_HEAD);
        return;
      }
      if (this.lost) throw new Error('worker lost while waiting for room');
      // Full: wait for the core to consume (it notifies IN_TAIL)
      const r = Atomics.waitAsync(this.ctrl, C_IN_TAIL, tail | 0, 1000);
      if (r.async) await r.value;
    }
  }
}

// Reads a NUL-terminated string at `at`, not past `end`.
function readCString(u8: Uint8Array, at: number, end: number): { text: string; bytes: number } {
  let n = 0;
  while (at + n < end && u8[at + n] !== 0) n++;
  return { text: decoder.decode(u8.slice(at, at + n)), bytes: n };
}

// Cuts a text field at the protocol's cap.
function cap(bytes: Uint8Array, max: number): Uint8Array {
  return bytes.length > max ? bytes.subarray(0, max) : bytes;
}
