import { describe, it, expect } from 'vitest';
import { visibleEntries, splitLogpoint, entryText } from '@/lib/logView';
import type { LogEntry } from '@/state/logs.svelte';

const E = (cat: string, level: number, msg = 'm'): LogEntry => ({ ts: 0, cat, level, msg });

describe('visibleEntries', () => {
  const all = [E('cpu', 1), E('scsi', 5), E('logpoint', 0), E('cpu', 9)];

  it('lets everything through with no filter (the same array)', () => {
    expect(visibleEntries(all, { maxLevel: null, hidden: {} })).toBe(all);
  });

  it('keeps levels up to the limit', () => {
    expect(visibleEntries(all, { maxLevel: 1, hidden: {} }).map((e) => e.level)).toEqual([1, 0]);
  });

  it('drops hidden categories, and combines with the level limit', () => {
    expect(
      visibleEntries(all, { maxLevel: null, hidden: { cpu: true } }).map((e) => e.cat),
    ).toEqual(['scsi', 'logpoint']);
    expect(
      visibleEntries(all, { maxLevel: 5, hidden: { logpoint: true, cpu: false } }).map(
        (e) => e.cat,
      ),
    ).toEqual(['cpu', 'scsi']);
  });
});

// The core's logpoint lines (debug.c), split so the message leads.
describe('splitLogpoint', () => {
  it('splits a PC logpoint into its message and the PC', () => {
    expect(splitLogpoint('logpoint $002E005C: make_process Desktop')).toEqual({
      where: '$002E005C',
      message: 'make_process Desktop',
    });
  });

  it('splits a memory logpoint into its message and the access', () => {
    expect(splitLogpoint('logpoint WRITE $00FCE008.b value=$00: ctx latch')).toEqual({
      where: 'WRITE $00FCE008.b = $00',
      message: 'ctx latch',
    });
  });

  it('reads the message-less forms', () => {
    expect(splitLogpoint('logpoint hit at $00FE0AEA (hit count: 3)')).toEqual({
      where: '$00FE0AEA',
      message: 'hit (hit count: 3)',
    });
    expect(splitLogpoint('logpoint READ $00000400.l value=$12345678 pc=$00400010')).toEqual({
      where: 'pc $00400010',
      message: 'READ $00000400.l = $12345678',
    });
  });

  it('leaves any other line alone', () => {
    expect(splitLogpoint('scsi reset')).toBeNull();
  });

  it('writes an entry back as its log line', () => {
    expect(entryText(E('scsi', 2, 'reset'))).toBe('[scsi] 2 reset');
  });
});
