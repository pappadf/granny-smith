// The console's output model: records in, entries out -- values between
// their markers, errors from their annotations, one flush per frame,
// the entry cap.
import { describe, it, expect, beforeEach } from 'vitest';
import {
  ConsoleModel,
  commandsText,
  jobOutputText,
  normalisePaste,
  type ConsoleEntry,
} from '@/lib/consoleModel';

let frames: Array<() => void>;
let flushes: number;
let shown: readonly ConsoleEntry[];

function make(cap?: number): ConsoleModel {
  return new ConsoleModel({
    cap,
    schedule: (fn) => frames.push(fn),
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
  flushes = 0;
  shown = [];
});

describe('ConsoleModel entries', () => {
  it('renders each entry kind', () => {
    const m = make();
    m.command('echo hi');
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
  it("renders this console's error annotation at its place in the output", () => {
    const m = make();
    m.push({ kind: 'job_start', job: 1 });
    m.push({ kind: 'output', job: 1, text: 'before\n' });
    m.push({ kind: 'error', job: 1, lines: ['bad', 'worse'] });
    m.push({ kind: 'output', job: 1, text: 'after\n' });
    m.push({ kind: 'job_end', job: 1 });
    frame();
    expect(view()).toEqual([
      ['text', 'before'],
      ['error', 'bad\nworse'],
      ['text', 'after'],
    ]);
  });

  it('ends a partial line at the error', () => {
    const m = make();
    m.push({ kind: 'job_start', job: 1 });
    m.push({ kind: 'output', job: 1, text: 'partial' });
    m.push({ kind: 'error', job: 1, lines: ['bad'] });
    m.push({ kind: 'job_end', job: 1 });
    frame();
    expect(view()).toEqual([
      ['text', 'partial'],
      ['error', 'bad'],
    ]);
  });

  it('a truncated annotation is not shown: its full text comes on stderr', () => {
    const m = make();
    m.push({ kind: 'job_start', job: 1 });
    m.push({ kind: 'error', job: 1, lines: ['cut'], truncated: true });
    m.push({ kind: 'stderr', line: 'the full text' });
    m.push({ kind: 'job_end', job: 1 });
    frame();
    expect(view()).toEqual([['stderr', 'the full text']]);
  });

  it('stderr lines are shown as they come, during a job or not', () => {
    const m = make();
    m.push({ kind: 'stderr', line: 'one' });
    m.push({ kind: 'job_start', job: 1 });
    m.push({ kind: 'stderr', line: 'two' });
    m.push({ kind: 'job_end', job: 1 });
    frame();
    expect(view()).toEqual([
      ['stderr', 'one'],
      ['stderr', 'two'],
    ]);
  });

  it("shows another client's error as stderr", () => {
    const m = make();
    m.push({ kind: 'job_start', job: 1 });
    m.push({ kind: 'error', job: 7, lines: ['elsewhere'] });
    m.push({ kind: 'job_end', job: 1 });
    frame();
    expect(view()).toEqual([['stderr', 'elsewhere']]);
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

describe('ConsoleModel dispose', () => {
  it('cancels the scheduled frame and takes nothing more', () => {
    const cancelled: number[] = [];
    const m = new ConsoleModel({
      schedule: (fn) => frames.push(fn),
      cancel: (h) => cancelled.push(h),
      onFlush: (e) => (shown = e),
    });
    m.push({ kind: 'print', line: 'a' });
    m.dispose();
    expect(cancelled).toEqual([1]);
    m.push({ kind: 'print', line: 'b' });
    m.echo('c');
    frame();
    expect(frames).toEqual([]);
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
