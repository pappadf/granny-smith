import { describe, it, expect, beforeEach, vi } from 'vitest';
import { getOrCreateMachine, rotateMachine } from '@/lib/machineId';

beforeEach(() => {
  localStorage.clear();
  vi.useRealTimers();
});

describe('machineId', () => {
  it('mints a fresh identity on first call', () => {
    const m = getOrCreateMachine();
    expect(m.id).toMatch(/^[0-9a-f]{16}$/);
    expect(m.created).toMatch(/^\d{8}T\d{6}Z$/);
  });

  it('returns the same identity on subsequent calls', () => {
    const a = getOrCreateMachine();
    const b = getOrCreateMachine();
    expect(b).toEqual(a);
  });

  it('persists across reads via localStorage', () => {
    const a = getOrCreateMachine();
    const raw = localStorage.getItem('gs.checkpoint.machine');
    expect(raw).not.toBeNull();
    const parsed = JSON.parse(raw!);
    expect(parsed.id).toBe(a.id);
    expect(parsed.created).toBe(a.created);
  });

  it('rotateMachine writes a new identity', () => {
    const a = getOrCreateMachine();
    const b = rotateMachine();
    expect(b.id).not.toBe(a.id);
    const c = getOrCreateMachine();
    expect(c.id).toBe(b.id);
  });

  it('recovers gracefully from malformed localStorage', () => {
    localStorage.setItem('gs.checkpoint.machine', 'not json');
    const m = getOrCreateMachine();
    expect(m.id).toMatch(/^[0-9a-f]{16}$/);
  });
});
