// The console's output model: records in, entries out -- values between
// their markers, errors claiming their stderr lines, one flush per frame,
// the entry cap.
import { describe, it, expect, beforeEach } from 'vitest';
import {
  ConsoleModel,
  commandsText,
  jobOutputText,
  normalisePaste,
  ERROR_SETTLE_MS,
  type ConsoleEntry,
} from '@/lib/consoleModel';

let frames: Array<() => void>;
let timers: Array<{ fn: () => void; ms: number; live: boolean }>;
let flushes: number;
let shown: readonly ConsoleEntry[];

function make(cap?: number): ConsoleModel {
  return new ConsoleModel({
    cap,
    schedule: (fn) => frames.push(fn),
    setTimer: (fn, ms) => {
      const t = { fn, ms, live: true };
      timers.push(t);
      return () => (t.live = false);
    },
    onFlush: (e) => {
      flushes++;
      shown = e;
    },
  });
}

// Run the pending animation frame(s).
function frame(): void {
  const f = frames;
  frames = [];
  for (const fn of f) fn();
}

const view = () => shown.map((e) => [e.kind, e.text]);

beforeEach(() => {
  frames = [];
  timers = [];
  flushes = 0;
  shown = [];
});

describe('ConsoleModel entries', () => {
  it('renders each entry kind', () => {
    const m = make();
    m.command('echo hi', 'gs>');
    m.push({ kind: 'job_start', job: 7 });
    m.push({ kind: 'output', job: 7, text: 'hi\n' });
    m.push({ kind: 'job_end', job: 7 });
    m.push({ kind: 'print', line: 'booted' });
    m.push({ kind: 'stderr', line: 'warning: x' });
    m.echo('machine.cpu.d0 = 0x1');
    frame();
    expect(view()).toEqual([
      ['command', 'echo hi'],
      ['text', 'hi'],
      ['text', 'booted'],
      ['stderr', 'warning: x'],
      ['echo', 'machine.cpu.d0 = 0x1'],
    ]);
    expect(shown[0].prompt).toBe('gs>');
    expect(shown[1].job).toBe(7);
  });

  it('assembles output pieces into lines; a last partial line ends with the job', () => {
    const m = make();
    m.push({ kind: 'job_start', job: 1 });
    m.push({ kind: 'output', job: 1, text: 'ab' });
    m.push({ kind: 'output', job: 1, text: 'c\nde' });
    frame();
    expect(view()).toEqual([['text', 'abc']]);
    m.push({ kind: 'job_end', job: 1 });
    frame();
    expect(view()).toEqual([
      ['text', 'abc'],
      ['text', 'de'],
    ]);
  });

  it('a request capture (no job) is complete in itself', () => {
    const m = make();
    m.push({ kind: 'output', job: null, text: 'one\ntwo' });
    frame();
    expect(view()).toEqual([
      ['text', 'one'],
      ['text', 'two'],
    ]);
  });
});

describe('ConsoleModel values', () => {
  it('turns the text between value_begin and value into one entry', () => {
    const m = make();
    m.push({ kind: 'job_start', job: 2 });
    m.push({ kind: 'output', job: 2, text: 'before\n' });
    m.push({ kind: 'value_begin', job: 2 });
    m.push({ kind: 'output', job: 2, text: '[1, 2,\n 3]\n' });
    m.push({ kind: 'value', job: 2, json: [1, 2, 3] });
    m.push({ kind: 'output', job: 2, text: 'after\n' });
    m.push({ kind: 'job_end', job: 2 });
    frame();
    expect(view()).toEqual([
      ['text', 'before'],
      ['value', '[1, 2,\n 3]'],
      ['text', 'after'],
    ]);
    expect(shown[1].json).toEqual([1, 2, 3]);
  });

  it('holds a value split across frames until its marker', () => {
    const m = make();
    m.push({ kind: 'job_start', job: 3 });
    m.push({ kind: 'value_begin', job: 3 });
    m.push({ kind: 'output', job: 3, text: '0x0000' });
    frame();
    expect(view()).toEqual([]);
    m.push({ kind: 'output', job: 3, text: '002A\n' });
    frame();
    expect(view()).toEqual([]);
    m.push({ kind: 'value', job: 3, json: 42 });
    frame();
    expect(view()).toEqual([['value', '0x0000002A']]);
  });

  it('renders held text as text when the job ends without its value', () => {
    const m = make();
    m.push({ kind: 'job_start', job: 4 });
    m.push({ kind: 'value_begin', job: 4 });
    m.push({ kind: 'output', job: 4, text: 'partial\nvalue' });
    m.push({ kind: 'job_end', job: 4 });
    frame();
    expect(view()).toEqual([
      ['text', 'partial'],
      ['text', 'value'],
    ]);
  });

  it('a truncated value has text but no json', () => {
    const m = make();
    m.push({ kind: 'job_start', job: 5 });
    m.push({ kind: 'value_begin', job: 5 });
    m.push({ kind: 'output', job: 5, text: 'big\n' });
    m.push({ kind: 'value', job: 5 });
    frame();
    expect(shown[0].kind).toBe('value');
    expect('json' in shown[0]).toBe(false);
  });
});

describe('ConsoleModel errors', () => {
  const ASSERT = ['line 3: assert $x', 'ASSERT FAILED: $x'];

  it('claims stderr lines that arrived before the annotation', () => {
    const m = make();
    m.push({ kind: 'job_start', job: 9 });
    m.push({ kind: 'output', job: 9, text: 'ok\n' });
    m.push({ kind: 'stderr', line: ASSERT[0] });
    m.push({ kind: 'stderr', line: ASSERT[1] });
    m.push({ kind: 'error', job: 9, lines: ASSERT });
    m.push({ kind: 'output', job: 9, text: 'next\n' });
    m.push({ kind: 'job_end', job: 9 });
    frame();
    expect(view()).toEqual([
      ['text', 'ok'],
      ['error', ASSERT.join('\n')],
      ['text', 'next'],
    ]);
  });

  it('waits for stderr lines that arrive after the annotation, keeping order', () => {
    const m = make();
    m.push({ kind: 'job_start', job: 10 });
    m.push({ kind: 'error', job: 10, lines: ASSERT });
    m.push({ kind: 'output', job: 10, text: 'later\n' });
    frame();
    // The output behind the unresolved error waits: nothing re-renders later.
    expect(view()).toEqual([]);
    m.push({ kind: 'stderr', line: ASSERT[0] });
    m.push({ kind: 'stderr', line: ASSERT[1] });
    frame();
    expect(view()).toEqual([
      ['error', ASSERT.join('\n')],
      ['text', 'later'],
    ]);
  });

  it('stderr lines that arrive after the job ended are not shown twice', () => {
    // The worker's printErr travels apart from the job's records: in the
    // browser an unknown command's line lands after job_end.
    const m = make();
    m.push({ kind: 'job_start', job: 20 });
    m.push({ kind: 'error', job: 20, lines: ["line 1: unknown command or path 'dfsgdfg'"] });
    m.push({ kind: 'job_end', job: 20 });
    m.push({ kind: 'stderr', line: "line 1: unknown command or path 'dfsgdfg'" });
    frame();
    expect(view()).toEqual([['error', "line 1: unknown command or path 'dfsgdfg'"]]);
    // Also when the next job has started meanwhile (type-ahead).
    m.push({ kind: 'job_start', job: 21 });
    m.push({ kind: 'error', job: 21, lines: ['a', 'b'] });
    m.push({ kind: 'job_end', job: 21 });
    m.push({ kind: 'job_start', job: 22 });
    m.push({ kind: 'stderr', line: 'a' });
    m.push({ kind: 'stderr', line: 'b' });
    m.push({ kind: 'job_end', job: 22 });
    frame();
    expect(view().slice(1)).toEqual([['error', 'a\nb']]);
  });

  it('a late line past the settle time is shown', () => {
    const m = make();
    m.push({ kind: 'job_start', job: 23 });
    m.push({ kind: 'error', job: 23, lines: ['x'] });
    m.push({ kind: 'job_end', job: 23 });
    for (const t of timers.filter((t) => t.live && t.ms === ERROR_SETTLE_MS)) t.fn();
    m.push({ kind: 'stderr', line: 'x' });
    frame();
    expect(view()).toEqual([
      ['error', 'x'],
      ['stderr', 'x'],
    ]);
  });

  it('two identical errors in one job each claim their own run of lines', () => {
    const m = make();
    m.push({ kind: 'job_start', job: 11 });
    m.push({ kind: 'stderr', line: 'oops' });
    m.push({ kind: 'error', job: 11, lines: ['oops'] });
    m.push({ kind: 'output', job: 11, text: 'between\n' });
    m.push({ kind: 'error', job: 11, lines: ['oops'] });
    m.push({ kind: 'stderr', line: 'oops' });
    m.push({ kind: 'job_end', job: 11 });
    frame();
    expect(view()).toEqual([
      ['error', 'oops'],
      ['text', 'between'],
      ['error', 'oops'],
    ]);
  });

  it('unclaimed held lines become stderr at job end, in arrival order', () => {
    const m = make();
    m.push({ kind: 'job_start', job: 12 });
    m.push({ kind: 'stderr', line: 'warn a' });
    m.push({ kind: 'stderr', line: 'boom' });
    m.push({ kind: 'error', job: 12, lines: ['boom'] });
    m.push({ kind: 'stderr', line: 'warn b' });
    m.push({ kind: 'job_end', job: 12 });
    frame();
    expect(view()).toEqual([
      ['error', 'boom'],
      ['stderr', 'warn a'],
      ['stderr', 'warn b'],
    ]);
  });

  it('settles held lines 2 s after one arrives, while the job still runs', () => {
    const m = make();
    m.push({ kind: 'job_start', job: 13 });
    m.push({ kind: 'stderr', line: 'lonely' });
    frame();
    expect(view()).toEqual([]);
    const t = timers.find((x) => x.live);
    expect(t?.ms).toBe(ERROR_SETTLE_MS);
    t!.fn();
    frame();
    expect(view()).toEqual([['stderr', 'lonely']]);
  });

  it('an annotation whose lines never came renders its own lines at job end', () => {
    const m = make();
    m.push({ kind: 'job_start', job: 14 });
    m.push({ kind: 'error', job: 14, lines: ['parse error'] });
    m.push({ kind: 'job_end', job: 14 });
    frame();
    expect(view()).toEqual([['error', 'parse error']]);
  });

  it('stderr outside the console job is shown at once', () => {
    const m = make();
    m.push({ kind: 'stderr', line: 'boot warning' });
    frame();
    expect(view()).toEqual([['stderr', 'boot warning']]);
  });

  it("ignores another client's error annotation (its stderr already shows)", () => {
    const m = make();
    m.push({ kind: 'stderr', line: 'x' });
    m.push({ kind: 'error', job: 99, lines: ['x'] });
    frame();
    expect(view()).toEqual([['stderr', 'x']]);
  });
});

describe('ConsoleModel batching and cap', () => {
  it('flushes once per frame however many records arrive', () => {
    const m = make();
    for (let i = 0; i < 100; i++) m.push({ kind: 'print', line: `l${i}` });
    expect(frames.length).toBe(1);
    expect(flushes).toBe(0);
    frame();
    expect(flushes).toBe(1);
    expect(shown.length).toBe(100);
  });

  it('keeps existing entries (same objects) when appending', () => {
    const m = make();
    m.push({ kind: 'print', line: 'a' });
    frame();
    const first = shown[0];
    m.push({ kind: 'print', line: 'b' });
    frame();
    expect(shown[0]).toBe(first);
  });

  it('drops the oldest entries beyond the cap', () => {
    const m = make(5000);
    for (let i = 0; i < 5010; i++) m.push({ kind: 'print', line: `l${i}` });
    frame();
    expect(shown.length).toBe(5000);
    expect(shown[0].text).toBe('l10');
    expect(shown[4999].text).toBe('l5009');
  });

  it('clear empties the list', () => {
    const m = make();
    m.push({ kind: 'print', line: 'a' });
    frame();
    m.clear();
    expect(shown).toEqual([]);
  });
});

describe('copy and paste helpers', () => {
  const e = (id: number, kind: ConsoleEntry['kind'], text: string, job: number | null = null) =>
    ({ id, kind, text, job }) as ConsoleEntry;

  it('Copy as commands: the command statements, one per line', () => {
    const list = [e(1, 'command', 'let a = 1'), e(2, 'text', 'x', 5), e(3, 'command', 'echo $a')];
    expect(commandsText(list)).toBe('let a = 1\necho $a');
  });

  it("Copy output: one job's text, value and error entries", () => {
    const list = [
      e(1, 'command', 'x'),
      e(2, 'text', 'a', 5),
      e(3, 'value', 'b', 5),
      e(4, 'text', 'c', 6),
      e(5, 'error', 'd', 5),
    ];
    expect(jobOutputText(list, 5)).toBe('a\nb\nd');
    expect(jobOutputText(list, null)).toBe('');
  });

  it('normalises pasted text', () => {
    expect(normalisePaste('› echo a  \r\n> echo b\r\n\r\n')).toBe('echo a\necho b');
    expect(normalisePaste('echo "x"\n')).toBe('echo "x"');
    expect(normalisePaste('a > b')).toBe('a > b');
  });
});
