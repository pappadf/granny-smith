import { describe, it, expect } from 'vitest';
import { urlSchedulerMode, parseUrlMediaParams, hasUrlMedia } from '@/bus/urlMedia';

// ?speed= reaches the core through the toolbar's mode.
describe('urlSchedulerMode', () => {
  it.each([
    ['paced', 'live'],
    ['accelerated', 'accel'],
    ['turbo', 'turbo'],
  ] as const)('%s -> %s', (speed, mode) => {
    expect(urlSchedulerMode(speed)).toBe(mode);
  });

  it('ignores an absent or unknown value', () => {
    expect(urlSchedulerMode(null)).toBeNull();
    expect(urlSchedulerMode('warp')).toBeNull();
  });

  it('rejects the retired aliases', () => {
    for (const legacy of ['realtime', 'real', 'hardware', 'hw', 'accel', 'max'])
      expect(urlSchedulerMode(legacy)).toBeNull();
  });
});

// Any media parameter starts URL processing: a CD or a vROM alone
// used to be ignored.
describe('hasUrlMedia', () => {
  it.each(['cd=x.iso', 'vrom=x.vrom', 'rom=x.rom', 'fd0=x.dsk', 'hd1=x.img'])('%s', (q) => {
    expect(hasUrlMedia(parseUrlMediaParams(new URLSearchParams(q)))).toBe(true);
  });

  it('speed alone is not media', () => {
    expect(hasUrlMedia(parseUrlMediaParams(new URLSearchParams('speed=turbo')))).toBe(false);
  });
});
