// @vitest-environment node
//
// The platen worker's ring loop (src/printer/platenRing.ts) against the
// real interpreter module (`make platen-module` → build/platen-<version>.js,
// loaded under Node) with the rings laid out in a plain SharedArrayBuffer
// per laserwriter_ring_protocol.h — no Worker, no emulator.  This test
// plays the core: it writes OPEN / FEED / FINISH into the outbound ring
// the way laserwriter_transport_ring.c does, runs the loop, and reads the
// answers back with the same PAD8 framing.  Skipped (loudly) when the
// module has not been built: `npm test` runs before `make` in a fresh
// checkout.

import { describe, it, expect, beforeAll } from 'vitest';
import { existsSync, readdirSync, readFileSync } from 'node:fs';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { join } from 'node:path';
import * as P from '@/printer/platenProtocol';
import { loadPlatenModule, PlatenLib } from '@/printer/platenLib';
import { PlatenRing, documentName, type DocumentMsg } from '@/printer/platenRing';

const repoRoot = fileURLToPath(new URL('../../../../', import.meta.url));
const buildDir = join(repoRoot, 'build');
const moduleJs = existsSync(buildDir)
  ? readdirSync(buildDir)
      .filter((f) => /^platen-.*\.js$/.test(f))
      .map((f) => join(buildDir, f))[0]
  : undefined;
if (!moduleJs)
  console.warn('platenRing.test: build/platen-*.js not found; run `make platen-module`');

// The bridge's identity and prelude (laserwriter_job.c / laserwriter_prelude.ps).
const IDENTITY = [
  { key: 'product', value: '(LaserWriter II NT)' },
  { key: 'version', value: '(47.0)' },
  { key: 'revision', value: '0' },
];
const PRELUDE = new Uint8Array(
  readFileSync(join(repoRoot, 'src/core/network/laserwriter_prelude.ps')),
);
const PROGRAM = new TextEncoder().encode('0.5 setgray 100 100 200 300 rectfill showpage\n');

const CTRL_AT = 64; // the control block, 64-byte aligned inside the region

// A region laid out as laserwriter_transport_ring.c lays it out, with the
// cursors of either ring optionally started at an offset (to place a
// record near the ring's end).
interface Region {
  sab: SharedArrayBuffer;
  ctrl: Int32Array;
  out: P.Ring;
  inb: P.Ring;
  outWr: number; // the core's outbound write cursor
  inRd: number; // the core's inbound read cursor
}

function makeRegion(outSize: number, inSize: number, outStart = 0, inStart = 0): Region {
  const ctrlBytes = P.CTRL_WORDS * 4;
  const sab = new SharedArrayBuffer(CTRL_AT + ctrlBytes + outSize + inSize);
  const ctrl = new Int32Array(sab, CTRL_AT, P.CTRL_WORDS);
  ctrl[P.C_MAGIC] = P.MAGIC;
  ctrl[P.C_VERSION] = P.PROTOCOL_VERSION;
  ctrl[P.C_OUT_OFF] = ctrlBytes;
  ctrl[P.C_OUT_SIZE] = outSize;
  ctrl[P.C_IN_OFF] = ctrlBytes + outSize;
  ctrl[P.C_IN_SIZE] = inSize;
  ctrl[P.C_OUT_HEAD] = outStart;
  ctrl[P.C_OUT_TAIL] = outStart;
  ctrl[P.C_IN_HEAD] = inStart;
  ctrl[P.C_IN_TAIL] = inStart;
  const u8 = new Uint8Array(sab);
  return {
    sab,
    ctrl,
    out: { u8, base: CTRL_AT + ctrlBytes, size: outSize },
    inb: { u8, base: CTRL_AT + ctrlBytes + outSize, size: inSize },
    outWr: outStart,
    inRd: inStart,
  };
}

// The core's side of the outbound ring: one record, published.
function coreWrite(r: Region, kind: number, words: number[], texts: Uint8Array[]): void {
  const tail = Atomics.load(r.ctrl, P.C_OUT_TAIL) >>> 0;
  const wr = P.ringWrite(r.out, r.outWr, tail, kind, words, texts);
  expect(wr).toBeGreaterThanOrEqual(0);
  r.outWr = wr;
  Atomics.store(r.ctrl, P.C_OUT_HEAD, wr | 0);
  Atomics.notify(r.ctrl, P.C_OUT_HEAD);
}

function coreOpen(r: Region, jobId: number): void {
  const enc = new TextEncoder();
  const idBlock = enc.encode(IDENTITY.map((e) => `${e.key}\0${e.value}\0`).join(''));
  const body = new Uint8Array(idBlock.length + PRELUDE.length);
  body.set(idBlock, 0);
  body.set(PRELUDE, idBlock.length);
  const words = new Array<number>(P.OPEN_WORDS).fill(0);
  words[P.OPEN_JOB] = jobId;
  words[P.OPEN_COMPRESS] = 1;
  words[P.OPEN_EMBED] = 0;
  words[P.OPEN_BUDGET_L] = 100000000;
  words[P.OPEN_BUDGET_H] = 0;
  words[P.OPEN_PASSWORD] = 0;
  words[P.OPEN_ID_COUNT] = IDENTITY.length;
  words[P.OPEN_ID_BYTES] = idBlock.length;
  words[P.OPEN_PRELUDE] = PRELUDE.length;
  coreWrite(r, P.R_OPEN, words, [body]);
}

function openRecordBytes(): number {
  const idBytes = new TextEncoder().encode(
    IDENTITY.map((e) => `${e.key}\0${e.value}\0`).join(''),
  ).length;
  return P.recordBytes(P.OPEN_WORDS, [idBytes + PRELUDE.length]);
}

function coreFeed(r: Region, jobId: number, seq: number, bytes: Uint8Array): void {
  coreWrite(r, P.R_FEED, [jobId, seq, bytes.length], [bytes]);
}

function coreFinish(r: Region, jobId: number, title: string): void {
  const t = new TextEncoder().encode(title);
  coreWrite(r, P.R_FINISH, [jobId, t.length], [t]);
}

// An inbound record as the core would decode it.
interface Answer {
  kind: number;
  len: number;
  words: number[];
  texts: Uint8Array[];
}

// The core's side of the inbound ring: every record published so far
// (PADs included, so a test can see one), consumed and acknowledged.
function coreRead(r: Region): Answer[] {
  const out: Answer[] = [];
  for (;;) {
    const head = Atomics.load(r.ctrl, P.C_IN_HEAD) >>> 0;
    if ((head - r.inRd) >>> 0 < P.HDR_BYTES) break;
    const rec = P.ringRead(r.inb, r.inRd, head);
    expect(rec.len & 7).toBe(0);
    const u8 = r.inb.u8;
    const words: number[] = [];
    const texts: Uint8Array[] = [];
    let p = rec.payload;
    const wordCount =
      rec.kind === P.R_OPENED
        ? P.OPENED_WORDS
        : rec.kind === P.R_OPEN_FAILED
          ? P.OPEN_FAILED_WORDS
          : rec.kind === P.R_FED
            ? P.FED_WORDS
            : rec.kind === P.R_FINISHED
              ? P.FINISHED_WORDS
              : 0;
    for (let i = 0; i < wordCount; i++) {
      words.push(P.getU32(u8, p));
      p += 4;
    }
    const textLens =
      rec.kind === P.R_OPEN_FAILED
        ? [words[P.OPEN_FAILED_TEXT]]
        : rec.kind === P.R_FED
          ? [words[P.FED_REPLY_LEN], words[P.FED_ERROR_LEN]]
          : rec.kind === P.R_FINISHED
            ? [
                words[P.FINISHED_ERRNAME_LEN],
                words[P.FINISHED_OFFEND_LEN],
                words[P.FINISHED_REPLY_LEN],
                words[P.FINISHED_ERROR_LEN],
              ]
            : [];
    for (const n of textLens) {
      texts.push(u8.slice(p, p + n));
      p += P.pad4(n);
    }
    out.push({ kind: rec.kind, len: rec.len, words, texts });
    r.inRd = (r.inRd + rec.len) >>> 0;
    Atomics.store(r.ctrl, P.C_IN_TAIL, r.inRd | 0);
    Atomics.notify(r.ctrl, P.C_IN_TAIL);
  }
  return out;
}

// A host that keeps what the loop posts.
function recorder(): {
  host: { post(msg: DocumentMsg, transfer: Transferable[]): void };
  docs: DocumentMsg[];
} {
  const docs: DocumentMsg[] = [];
  return { host: { post: (msg) => docs.push(msg) }, docs };
}

describe.skipIf(!moduleJs)('platen ring loop', () => {
  let lib: PlatenLib;

  beforeAll(async () => {
    const js = moduleJs!;
    const wasm = js.replace(/\.js$/, '.wasm');
    lib = new PlatenLib(await loadPlatenModule(pathToFileURL(js).href, pathToFileURL(wasm).href));
  }, 60_000);

  it('runs OPEN, FEED, FINISH through the rings and posts the PDF', async () => {
    const r = makeRegion(64 << 10, 64 << 10);
    // The bridge writes before the worker attaches (the module is fetched
    // on the first job); the loop must pick these up
    coreOpen(r, 1);
    coreFeed(r, 1, 7, PROGRAM);
    coreFinish(r, 1, 'Test Page');
    const rec = recorder();
    const ring = new PlatenRing(r.sab, CTRL_AT, lib, rec.host);
    expect(Atomics.load(r.ctrl, P.C_STATUS)).toBe(P.STATUS_ATTACHED);
    // Run the real loop (Atomics.waitAsync parks it once the ring is
    // drained); stop it once the three answers are in
    const loop = ring.run();
    const answers: Answer[] = [];
    for (let i = 0; i < 200 && answers.length < 3; i++) {
      answers.push(...coreRead(r));
      if (answers.length < 3) await new Promise((res) => setTimeout(res, 25));
    }
    ring.stop();
    await loop;

    expect(answers.map((a) => a.kind)).toEqual([P.R_OPENED, P.R_FED, P.R_FINISHED]);
    expect(answers[0].words[P.OPENED_JOB]).toBe(1);
    expect(answers[0].len).toBe(P.pad8(P.HDR_BYTES + 4 * P.OPENED_WORDS));
    const fed = answers[1];
    expect(fed.words[P.FED_JOB]).toBe(1);
    expect(fed.words[P.FED_SEQ]).toBe(7);
    expect(fed.words[P.FED_STATUS]).toBe(P.FEED_WAITING);
    expect(fed.words[P.FED_PAGES]).toBe(1);
    expect(fed.words[P.FED_FLAGS]).toBe(0);
    expect(fed.len).toBe(
      P.recordBytes(P.FED_WORDS, [fed.words[P.FED_REPLY_LEN], fed.words[P.FED_ERROR_LEN]]),
    );
    const fin = answers[2];
    expect(fin.words[P.FINISHED_JOB]).toBe(1);
    expect(fin.words[P.FINISHED_OUTCOME]).toBe(P.OUTCOME_OK);
    expect(fin.words[P.FINISHED_PAGES]).toBe(1);
    expect(fin.words[P.FINISHED_ERRNAME_LEN]).toBe(0);
    expect(fin.texts[3].length).toBe(fin.words[P.FINISHED_ERROR_LEN]);
    // Statistics and cursors
    expect(Atomics.load(r.ctrl, P.C_STAT_JOBS)).toBe(1);
    expect(Atomics.load(r.ctrl, P.C_STAT_FEEDS)).toBe(1);
    expect(Atomics.load(r.ctrl, P.C_OUT_TAIL) >>> 0).toBe(r.outWr);
    // The document went to the page, not the ring
    expect(rec.docs.length).toBe(1);
    const doc = rec.docs[0];
    expect(doc.jobId).toBe(1);
    expect(doc.title).toBe('Test Page');
    expect(doc.name).toBe('00001-Test_Page.pdf');
    expect(doc.pages).toBe(1);
    expect(new TextDecoder().decode(doc.pdf.subarray(0, 5))).toBe('%PDF-');
  }, 30_000);

  it('a record that would cross the ring end is preceded by a PAD and restarts at 0', async () => {
    const outSize = 8192;
    const inSize = 4096;
    // Start the outbound cursor so OPEN ends 16 bytes short of the end:
    // the FEED (24 bytes at least) cannot fit and needs a 16-byte PAD.
    // Start the inbound cursor 24 bytes short: the worker's OPENED (16
    // bytes) leaves 8, and its FED needs a PAD of just a header.
    const outStart = outSize - openRecordBytes() - 16;
    const inStart = inSize - 24;
    const r = makeRegion(outSize, inSize, outStart, inStart);
    coreOpen(r, 2);
    expect(r.outWr).toBe(outSize - 16);
    coreFeed(r, 2, 9, PROGRAM);
    // The PAD sits where the FEED would have started; the FEED is at 0
    const pad = P.ringRead(r.out, outSize - 16, r.outWr);
    expect(pad.kind).toBe(P.R_PAD);
    expect(pad.len).toBe(16);
    const feedAt0 = P.ringRead(r.out, outSize, r.outWr);
    expect(feedAt0.kind).toBe(P.R_FEED);
    expect(r.outWr).toBe(outSize + feedAt0.len);
    coreFinish(r, 2, '');

    const rec = recorder();
    const ring = new PlatenRing(r.sab, CTRL_AT, lib, rec.host);
    expect(await ring.service()).toBe(true);
    const answers = coreRead(r);
    // The worker consumed the PAD and every record after it
    expect(Atomics.load(r.ctrl, P.C_OUT_TAIL) >>> 0).toBe(r.outWr);
    expect(answers.map((a) => a.kind)).toEqual([P.R_OPENED, P.R_PAD, P.R_FED, P.R_FINISHED]);
    expect(answers[1].len).toBe(8); // the worker's own PAD, reaching the inbound ring's end
    expect(answers[2].words[P.FED_SEQ]).toBe(9);
    expect(answers[2].words[P.FED_STATUS]).toBe(P.FEED_WAITING);
    expect(answers[3].words[P.FINISHED_OUTCOME]).toBe(P.OUTCOME_OK);
    expect(rec.docs[0].name).toBe('00002-untitled.pdf');
  }, 30_000);

  it('refuses a control block of another protocol version', () => {
    const r = makeRegion(4096, 4096);
    r.ctrl[P.C_VERSION] = P.PROTOCOL_VERSION + 1;
    expect(() => new PlatenRing(r.sab, CTRL_AT, lib, recorder().host)).toThrow(/version/);
    expect(Atomics.load(r.ctrl, P.C_STATUS)).toBe(P.STATUS_DETACHED);
  });

  it('answers a feed for a job it does not hold with FED failed', async () => {
    const r = makeRegion(4096, 4096);
    coreFeed(r, 5, 1, PROGRAM);
    const ring = new PlatenRing(r.sab, CTRL_AT, lib, recorder().host);
    await ring.service();
    const [fed] = coreRead(r);
    expect(fed.kind).toBe(P.R_FED);
    expect(fed.words[P.FED_JOB]).toBe(5);
    expect(fed.words[P.FED_STATUS]).toBe(P.FEED_FAILED);
  });
});

describe('documentName', () => {
  it('names the download like the headless print directory does', () => {
    expect(documentName(3, 'Macintosh HD')).toBe('00003-Macintosh_HD.pdf');
    expect(documentName(12, '  Report (draft) / v2.1 ')).toBe('00012-Report_draft_v2.1.pdf');
    expect(documentName(7, '')).toBe('00007-untitled.pdf');
    expect(documentName(1, '///')).toBe('00001-untitled.pdf');
    expect(documentName(123456, 'x'.repeat(100))).toBe(`123456-${'x'.repeat(P.TITLE_MAX)}.pdf`);
  });
});

describe('record framing helpers', () => {
  it('pads text fields to 4 and records to 8', () => {
    expect(P.pad4(0)).toBe(0);
    expect(P.pad4(1)).toBe(4);
    expect(P.pad4(5)).toBe(8);
    expect(P.pad8(8)).toBe(8);
    expect(P.pad8(9)).toBe(16);
    expect(P.recordBytes(1, [])).toBe(16);
    expect(P.recordBytes(2, [5, 3])).toBe(P.pad8(8 + 8 + 8 + 4));
  });

  it('refuses a len that is not a multiple of 8', () => {
    const r = makeRegion(1024, 1024);
    P.putU32(r.out.u8, r.out.base, P.R_FEED);
    P.putU32(r.out.u8, r.out.base + 4, 20);
    expect(() => P.ringRead(r.out, 0, 24)).toThrow(/corrupt/);
  });
});
