// The console's output model: the records the core sends (Module.print /
// printErr lines, a job's output pieces, its annotation records) become the
// entries the Terminal's console renders.  Pure -- no DOM, no timers of its
// own (the caller supplies a frame scheduler and a clock), so
// tests/unit/consoleModel.test.ts drives it directly.
//
// Entries:
//   command  the submitted input
//   text     a job's printed text, or a Module.print line outside a job
//   stderr   a printErr line, or another client's error
//   value    the text a job printed between `value_begin` and `value`
//   error    an `error` annotation of this console's job
//   echo     a statement another surface ran on the user's behalf
//
// Values: after `value_begin` for a job, its text is held until the
// `value` annotation; the held text becomes one `value` entry.  If the job
// ends first, the held text becomes `text`.
//
// Errors: the core writes a job's error once, as an `error` annotation in
// the job's ordered record stream; it goes to stderr only when no record
// can carry it.  A `truncated` annotation (the shortened form of an error
// too large for a record) is such a case: its full text comes on stderr,
// so the annotation itself is not shown.
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
  | { kind: 'error'; job: number; lines: string[]; truncated?: boolean }
  | { kind: 'job_start'; job: number }
  | { kind: 'job_end'; job: number };

export interface ConsoleModelOptions {
  cap?: number;
  // Runs `fn` once, at the next frame (requestAnimationFrame in the view).
  schedule: (fn: () => void) => void;
  // Receives the entries to show after each frame's batch.
  onFlush: (entries: readonly ConsoleEntry[]) => void;
}

export const CONSOLE_CAP = 5000;

interface JobState {
  tail: string; // a partial line not yet ended
  holding: boolean; // between value_begin and value
  held: string; // the value's text so far
}

export class ConsoleModel {
  private entries: ConsoleEntry[] = [];
  private pending: ConsoleEntry[] = [];
  private scheduled = false;
  private nextId = 1;
  private readonly cap: number;
  private jobs = new Map<number, JobState>();
  private loose: JobState = { tail: '', holding: false, held: '' };
  // This console's running job.
  private active: number | null = null;

  constructor(private readonly opts: ConsoleModelOptions) {
    this.cap = opts.cap ?? CONSOLE_CAP;
  }

  // What is committed (the view's list).
  list(): readonly ConsoleEntry[] {
    return this.entries;
  }

  command(text: string, spans?: readonly HlSpan[]): void {
    this.add('command', text, null, undefined, spans);
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
        this.add('stderr', r.line, null);
        return;
      case 'output':
        this.output(r.job, r.text);
        return;
      case 'value_begin': {
        const s = this.job(r.job);
        this.endLine(s, r.job);
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
        this.add('value', text, r.job, r.json);
        return;
      }
      case 'error': {
        if (r.truncated) return; // the full text comes on stderr
        const s = this.job(r.job);
        this.endLine(s, r.job);
        this.add(r.job === this.active ? 'error' : 'stderr', r.lines.join('\n'), r.job);
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
      s = { tail: '', holding: false, held: '' };
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
    for (const line of lines) this.add('text', line, job);
    // A request's capture (job null) is complete in itself.
    if (job === null) this.endLine(s, null);
  }

  // Ends a partial line (at a value/error boundary, or the job's end).
  private endLine(s: JobState, job: number | null): void {
    if (s.tail) {
      this.add('text', s.tail, job);
      s.tail = '';
    }
  }

  private endJob(id: number): void {
    if (id !== this.active) return;
    const s = this.job(id);
    if (s.holding) {
      // No `value` came: what was held is plain text.
      s.holding = false;
      const text = s.held.endsWith('\n') ? s.held.slice(0, -1) : s.held;
      s.held = '';
      if (text) for (const line of text.split('\n')) this.add('text', line, id);
    }
    this.endLine(s, id);
    this.jobs.delete(id);
    this.active = null;
  }

  private add(
    kind: EntryKind,
    text: string,
    job: number | null,
    json?: unknown,
    spans?: readonly HlSpan[],
  ) {
    const e: ConsoleEntry = { id: this.nextId++, kind, text, job };
    if (json !== undefined) (e as { json?: unknown }).json = json;
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
    this.endLine(this.loose, null);
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
