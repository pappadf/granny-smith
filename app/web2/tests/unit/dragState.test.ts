import { describe, it, expect } from 'vitest';
import { nextDragState, isOutsideViewport } from '@/lib/dragState';

describe('nextDragState', () => {
  it('idle + enter(hasFiles) → active', () => {
    expect(nextDragState('idle', { kind: 'enter', hasFiles: true })).toBe('active');
  });

  it('idle + enter(no files) stays idle', () => {
    expect(nextDragState('idle', { kind: 'enter', hasFiles: false })).toBe('idle');
  });

  it('active + over-display → display', () => {
    expect(nextDragState('active', { kind: 'over-display' })).toBe('display');
  });

  it('active + over-fs-tree → fs-tree', () => {
    expect(nextDragState('active', { kind: 'over-fs-tree' })).toBe('fs-tree');
  });

  it('display + over-other → active (highlight cleared)', () => {
    expect(nextDragState('display', { kind: 'over-other' })).toBe('active');
  });

  it('idle + over-* stays idle (no spurious entry)', () => {
    expect(nextDragState('idle', { kind: 'over-display' })).toBe('idle');
    expect(nextDragState('idle', { kind: 'over-fs-tree' })).toBe('idle');
    expect(nextDragState('idle', { kind: 'over-other' })).toBe('idle');
  });

  it('any state + drop / end / viewport-exit / leave-all → idle', () => {
    for (const s of ['active', 'display', 'fs-tree'] as const) {
      expect(nextDragState(s, { kind: 'drop' })).toBe('idle');
      expect(nextDragState(s, { kind: 'end' })).toBe('idle');
      expect(nextDragState(s, { kind: 'viewport-exit' })).toBe('idle');
      expect(nextDragState(s, { kind: 'leave-all' })).toBe('idle');
    }
  });
});

describe('isOutsideViewport', () => {
  it('returns true for negative coords', () => {
    expect(isOutsideViewport(-1, 100)).toBe(true);
    expect(isOutsideViewport(100, -1)).toBe(true);
  });
  it('returns false for in-bounds coords', () => {
    expect(isOutsideViewport(100, 100)).toBe(false);
  });
});
