// The console's output model: the records the core sends (Module.print /
// printErr lines, a job's output pieces, its annotation records) become the
// entries the Terminal's console renders.  Pure -- no DOM, no timers of its
// own (the caller supplies a frame scheduler and a clock), so
// tests/unit/consoleModel.test.ts drives it directly.
//
// Entries:
//   command  the submitted input
//   text     a job's printed text, or a Module.print line outside a job
//   stderr   a printErr line not claimed by an `error` annotation
//   value    the text a job printed between `value_begin` and `value`
//   error    an `error` annotation with the stderr lines it claimed
//   echo     a statement another surface ran on the user's behalf
//
// Values: after `value_begin` for a job, its text is held until the
// `value` annotation; the held text becomes one `value` entry.  If the job
// ends first, the held text becomes `text`.
//
// Errors: while this console's job runs, printErr lines are held.  An
// `error` annotation claims the earliest run of held lines equal to its
// `lines`; output after an annotation waits behind it until it has claimed
// its lines, so the error entry lands at the annotation's position without
// re-rendering anything.  At job end -- or 2 s after a held line arrived --
// the rest is settled: an annotation whose lines never came renders its own
// lines, and unclaimed held lines become `stderr` entries in arrival order.
// The worker's printErr lines travel apart from the job's records and can
// land after the job's end: an error committed without its lines remembers
// them, and matching stderr lines that come within the settle time are
// absorbed rather than shown a second time.
//
// New entries are buffered and handed to `onFlush` once per frame; at most
// `cap` entries are kept (the oldest dropped).

import type { HlSpan } from '@/lib/highlight';

export type EntryKind = 'command' | 'text' | 'stderr' | 'value' | 'error' | 'echo';

export interface ConsoleEntry {
  readonly id: number;
  readonly kind: EntryKind;
  // The text as displayed (lines joined by '\n').
  readonly text: string;
  // The job whose output this is, when known.
  readonly job: number | null;
  // `value`: the tagged JSON of the value (absent when truncated).
  readonly json?: unknown;
  // `command`: the prompt the command was typed at.
  readonly prompt?: string;
  // `command`: its syntax colours when it was submitted (shell.highlight,
  // UTF-16 offsets into `text`).
  readonly spans?: readonly HlSpan[];
}

export type ConsoleRecord =
  | { kind: 'print'; line: string }
  | { kind: 'stderr'; line: string }
  | { kind: 'output'; text: string; job: number | null }
  | { kind: 'value_begin'; job: number }
  | { kind: 'value'; job: number; json?: unknown }
  | { kind: 'error'; job: number; lines: string[] }
  | { kind: 'job_start'; job: number }
  | { kind: 'job_end'; job: number };

export interface ConsoleModelOptions {
  cap?: number;
  // Runs `fn` once, at the next frame (requestAnimationFrame in the view).
  schedule: (fn: () => void) => void;
  // Arms a one-shot timer; returns its cancel.
  setTimer: (fn: () => void, ms: number) => () => void;
  // Receives the entries to show after each frame's batch.
  onFlush: (entries: readonly ConsoleEntry[]) => void;
}

export const CONSOLE_CAP = 5000;
export const ERROR_SETTLE_MS = 2000;

// One item of a job's ordered output: a finished line, a finished value, or
// an error annotation still looking for its stderr lines.
type Item =
  | { t: 'text'; text: string }
  | { t: 'value'; text: string; json?: unknown }
  | { t: 'error'; lines: string[] };

interface JobState {
  tail: string; // a partial line not yet ended
  holding: boolean; // between value_begin and value
  held: string; // the value's text so far
  queue: Item[];
}

export class ConsoleModel {
  private entries: ConsoleEntry[] = [];
  private pending: ConsoleEntry[] = [];
  private scheduled = false;
  private nextId = 1;
  private readonly cap: number;
  private jobs = new Map<number, JobState>();
  private loose: JobState = { tail: '', holding: false, held: '', queue: [] };
  // This console's running job, whose stderr is held.
  private active: number | null = null;
  private heldErr: string[] = [];
  private cancelSettle: (() => void) | null = null;
  // Lines of errors committed before their stderr arrived (see above).
  private late: string[] = [];
  private cancelLate: (() => void) | null = null;

  constructor(private readonly opts: ConsoleModelOptions) {
    this.cap = opts.cap ?? CONSOLE_CAP;
  }

  // What is committed (the view's list).
  list(): readonly ConsoleEntry[] {
    return this.entries;
  }

  command(text: string, prompt = '', spans?: readonly HlSpan[]): void {
    this.add('command', text, null, undefined, prompt, spans);
  }

  echo(text: string): void {
    this.add('echo', text, null);
  }

  clear(): void {
    this.entries = [];
    this.pending = [];
    this.opts.onFlush(this.entries);
  }

  push(r: ConsoleRecord): void {
    switch (r.kind) {
      case 'print':
        this.add('text', r.line, null);
        return;
      case 'stderr':
        if (this.late.length && this.late[0] === r.line) {
          this.late.shift();
          return;
        }
        if (this.active === null) {
          this.add('stderr', r.line, null);
          return;
        }
        this.heldErr.push(r.line);
        this.drain(this.active, this.job(this.active));
        this.armSettle();
        return;
      case 'output':
        this.output(r.job, r.text);
        return;
      case 'value_begin': {
        const s = this.job(r.job);
        this.endLine(s, false);
        s.holding = true;
        s.held = '';
        return;
      }
      case 'value': {
        const s = this.job(r.job);
        if (!s.holding) return;
        s.holding = false;
        const text = s.held.endsWith('\n') ? s.held.slice(0, -1) : s.held;
        s.held = '';
        s.queue.push({ t: 'value', text, json: r.json });
        this.drain(r.job, s);
        return;
      }
      case 'error': {
        // Another client's error: its stderr was not held, so it is
        // already shown.
        if (r.job !== this.active) return;
        const s = this.job(r.job);
        this.endLine(s, false);
        s.queue.push({ t: 'error', lines: r.lines });
        this.drain(r.job, s);
        return;
      }
      case 'job_start':
        this.active = r.job;
        return;
      case 'job_end':
        this.endJob(r.job);
        return;
    }
  }

  // --- internals ---------------------------------------------------------

  // This console's job has its own state; everything else (request
  // captures, other clients' jobs) shares one whose lines end each frame.
  private job(id: number | null): JobState {
    if (id === null || id !== this.active) return this.loose;
    let s = this.jobs.get(id);
    if (!s) {
      s = { tail: '', holding: false, held: '', queue: [] };
      this.jobs.set(id, s);
    }
    return s;
  }

  private output(job: number | null, text: string): void {
    const s = this.job(job);
    if (s.holding) {
      s.held += text;
      return;
    }
    const all = s.tail + text;
    const lines = all.split('\n');
    s.tail = lines.pop() ?? '';
    for (const line of lines) s.queue.push({ t: 'text', text: line });
    // A request's capture (job null) is complete in itself.
    if (job === null) this.endLine(s, false);
    this.drain(job, s);
  }

  // Ends a partial line (at a value/error boundary, or the job's end).
  private endLine(s: JobState, drain: boolean): void {
    if (s.tail) {
      s.queue.push({ t: 'text', text: s.tail });
      s.tail = '';
    }
    if (drain) this.drain(null, s);
  }

  // Commits a job's queue in order up to the first error annotation that
  // has not yet found its stderr lines (unless `force`).
  private drain(job: number | null, s: JobState, force = false): void {
    while (s.queue.length) {
      const it = s.queue[0];
      if (it.t === 'error') {
        const claimed = this.claim(it.lines);
        if (!claimed && !force) return;
        if (!claimed) this.expectLate(it.lines);
        this.add('error', it.lines.join('\n'), job);
      } else if (it.t === 'value') {
        this.add('value', it.text, job, it.json);
      } else {
        this.add('text', it.text, job);
      }
      s.queue.shift();
    }
  }

  // Removes the earliest run of held stderr lines equal to `lines`.
  private claim(lines: string[]): boolean {
    if (!lines.length) return true;
    const h = this.heldErr;
    for (let i = 0; i + lines.length <= h.length; i++) {
      let ok = true;
      for (let k = 0; k < lines.length && ok; k++) ok = h[i + k] === lines[k];
      if (ok) {
        h.splice(i, lines.length);
        return true;
      }
    }
    return false;
  }

  // An error was committed without its stderr lines: absorb them if they
  // arrive within the settle time.
  private expectLate(lines: string[]): void {
    this.late.push(...lines);
    this.cancelLate?.();
    this.cancelLate = this.opts.setTimer(() => {
      this.cancelLate = null;
      this.late = [];
    }, ERROR_SETTLE_MS);
  }

  private armSettle(): void {
    if (this.cancelSettle) return;
    this.cancelSettle = this.opts.setTimer(() => {
      this.cancelSettle = null;
      this.settleErrors();
    }, ERROR_SETTLE_MS);
  }

  // Held stderr that no annotation has claimed becomes plain stderr.
  private settleErrors(): void {
    if (this.active !== null) this.drain(this.active, this.job(this.active), true);
    const rest = this.heldErr;
    this.heldErr = [];
    for (const line of rest) this.add('stderr', line, this.active);
  }

  private endJob(id: number): void {
    if (id !== this.active) return;
    const s = this.job(id);
    if (s.holding) {
      // No `value` came: what was held is plain text.
      s.holding = false;
      const text = s.held.endsWith('\n') ? s.held.slice(0, -1) : s.held;
      s.held = '';
      if (text) for (const line of text.split('\n')) s.queue.push({ t: 'text', text: line });
    }
    this.endLine(s, false);
    this.drain(id, s, true);
    this.jobs.delete(id);
    if (this.active === id) {
      this.cancelSettle?.();
      this.cancelSettle = null;
      this.settleErrors();
      this.active = null;
    }
  }

  private add(
    kind: EntryKind,
    text: string,
    job: number | null,
    json?: unknown,
    prompt?: string,
    spans?: readonly HlSpan[],
  ) {
    const e: ConsoleEntry = { id: this.nextId++, kind, text, job };
    if (json !== undefined) (e as { json?: unknown }).json = json;
    if (prompt !== undefined) (e as { prompt?: string }).prompt = prompt;
    if (spans?.length) (e as { spans?: readonly HlSpan[] }).spans = spans;
    this.pending.push(e);
    if (!this.scheduled) {
      this.scheduled = true;
      this.opts.schedule(() => this.flush());
    }
  }

  // Once per frame: append the batch, drop the oldest beyond the cap.
  flush(): void {
    // A job not run by this console (no job_end will come) ends its lines
    // with the batch.
    this.endLine(this.loose, true);
    this.scheduled = false;
    if (!this.pending.length) return;
    let next = this.entries.concat(this.pending);
    this.pending = [];
    if (next.length > this.cap) next = next.slice(next.length - this.cap);
    this.entries = next;
    this.opts.onFlush(this.entries);
  }
}

// --- Copy helpers -----------------------------------------------------------

// The statements of `command` entries, one per line.
export function commandsText(entries: readonly ConsoleEntry[]): string {
  return entries
    .filter((e) => e.kind === 'command')
    .map((e) => e.text)
    .join('\n');
}

// All text/value/error entries of one job.
export function jobOutputText(entries: readonly ConsoleEntry[], job: number | null): string {
  if (job === null) return '';
  return entries
    .filter((e) => e.job === job && (e.kind === 'text' || e.kind === 'value' || e.kind === 'error'))
    .map((e) => e.text)
    .join('\n');
}

// Paste normalisation: CRLF → LF, trailing whitespace per line removed, a
// leading "› " or "> " removed from each line.
export function normalisePaste(text: string): string {
  return text
    .replace(/\r\n?/g, '\n')
    .split('\n')
    .map((l) => l.replace(/\s+$/, '').replace(/^(›|>) /, ''))
    .join('\n')
    .replace(/\n+$/, '');
}
