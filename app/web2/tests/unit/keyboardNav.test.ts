import { describe, it, expect } from 'vitest';
import { cycleListSelection, listKeyFromEvent } from '@/lib/keyboardNav';

describe('cycleListSelection', () => {
  it('ArrowDown from -1 lands on 0', () => {
    expect(cycleListSelection(5, -1, 'ArrowDown')).toBe(0);
  });
  it('ArrowDown advances', () => {
    expect(cycleListSelection(5, 2, 'ArrowDown')).toBe(3);
  });
  it('ArrowUp retreats', () => {
    expect(cycleListSelection(5, 2, 'ArrowUp')).toBe(1);
  });
  it('clamps at boundaries by default (no wrap)', () => {
    expect(cycleListSelection(5, 4, 'ArrowDown')).toBe(4);
    expect(cycleListSelection(5, 0, 'ArrowUp')).toBe(0);
  });
  it('wraps when wrap=true', () => {
    expect(cycleListSelection(5, 4, 'ArrowDown', { wrap: true })).toBe(0);
    expect(cycleListSelection(5, 0, 'ArrowUp', { wrap: true })).toBe(4);
  });
  it('Home / End', () => {
    expect(cycleListSelection(5, 3, 'Home')).toBe(0);
    expect(cycleListSelection(5, 1, 'End')).toBe(4);
  });
  it('PageUp / PageDown use the pageSize step', () => {
    expect(cycleListSelection(30, 5, 'PageDown', { pageSize: 10 })).toBe(15);
    expect(cycleListSelection(30, 15, 'PageUp', { pageSize: 10 })).toBe(5);
    // Clamped at the end:
    expect(cycleListSelection(30, 25, 'PageDown', { pageSize: 10 })).toBe(29);
  });
  it('ArrowLeft / ArrowRight behave like Up / Down', () => {
    expect(cycleListSelection(5, 2, 'ArrowRight')).toBe(3);
    expect(cycleListSelection(5, 2, 'ArrowLeft')).toBe(1);
  });
  it('returns -1 on empty list', () => {
    expect(cycleListSelection(0, -1, 'ArrowDown')).toBe(-1);
  });
});

describe('listKeyFromEvent', () => {
  it('recognises the navigation keys', () => {
    expect(listKeyFromEvent(new KeyboardEvent('keydown', { key: 'ArrowDown' }))).toBe('ArrowDown');
    expect(listKeyFromEvent(new KeyboardEvent('keydown', { key: 'End' }))).toBe('End');
    expect(listKeyFromEvent(new KeyboardEvent('keydown', { key: 'PageUp' }))).toBe('PageUp');
  });
  it('returns null for other keys', () => {
    expect(listKeyFromEvent(new KeyboardEvent('keydown', { key: 'a' }))).toBeNull();
    expect(listKeyFromEvent(new KeyboardEvent('keydown', { key: 'Enter' }))).toBeNull();
  });
});
