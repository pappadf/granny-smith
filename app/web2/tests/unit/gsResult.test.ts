import { describe, it, expect } from 'vitest';
import { gsEval, gsOk, isGsError, gsErrorText } from '@/bus/emulator';

// The gsEval result contract: null is only a VK_NONE
// success; every failure — the core's or the bridge's — is an { error } shape.
describe('gsEval result contract', () => {
  it('reports a module that is not ready as a transport error, not null', async () => {
    const r = await gsEval('machine.cpu.pc');
    expect(r).toEqual({ error: 'emulator not ready', transport: true });
    expect(isGsError(r)).toBe(true);
    expect(gsOk(r)).toBe(false);
  });

  it('treats null as a successful method that returned nothing', () => {
    expect(gsOk(null)).toBe(true);
    expect(isGsError(null)).toBe(false);
  });

  it('treats a VK_BOOL false and a core error as failure', () => {
    expect(gsOk(false)).toBe(false);
    expect(gsOk({ error: "path 'x' did not resolve" })).toBe(false);
    expect(gsOk(true)).toBe(true);
    expect(gsOk(0)).toBe(true);
  });

  it('explains each failure shape', () => {
    expect(gsErrorText({ error: 'no such drive' })).toBe('no such drive');
    expect(gsErrorText(false)).toBe('the operation reported failure');
  });
});

describe('request size', () => {
  it('accepts what the core serves and refuses what it would reject', async () => {
    const { requestTooLarge } = await import('@/bus/emulator');
    const ARGS_MAX = 128 << 10;
    expect(requestTooLarge('machine.cpu.pc', '')).toBeNull();
    expect(requestTooLarge('x'.repeat(1023), '')).toBeNull();
    expect(requestTooLarge('x'.repeat(1024), '')).toMatch(/path too large/);
    expect(requestTooLarge('p', 'a'.repeat(ARGS_MAX))).toBeNull();
    expect(requestTooLarge('p', 'a'.repeat(ARGS_MAX + 1))).toMatch(/arguments too large/);
    // Measured in UTF-8 bytes, not characters: three-byte characters are
    // over the limit long before their count is.
    expect(requestTooLarge('p', '€'.repeat(ARGS_MAX / 3 + 1))).toMatch(/arguments too large/);
  });
});
