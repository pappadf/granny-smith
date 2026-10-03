import { describe, it, expect, vi, beforeEach } from 'vitest';
import { bridge } from '../helpers/bridgeMock';

vi.mock('@/bus/emulator', async () => (await import('../helpers/bridgeMock')).emulatorModule());

const {
  translateAddr,
  walkAddr,
  mapRange,
  readDescriptors,
  descriptorFormats,
  readMmuState,
  MAP_LIMIT,
} = await import('@/bus/mmu');

// The bus wrappers over machine.cpu.mmu's translate / walk / map /
// descriptor: the paths and arguments they send, and how they read the core's
// result shapes (debug_mmu.h), which are the same on every MMU kind.
describe('bus/mmu against the machine.cpu.mmu methods', () => {
  beforeEach(() => bridge.reset());

  it('translate reads phys / valid / via / access', async () => {
    bridge.reply('machine.cpu.mmu.translate', {
      phys: 0x40826cc0,
      valid: true,
      via: 'page',
      access: 'rw',
    });
    const t = await translateAddr(0x40826cc0, false);
    expect(t).toEqual({
      phys: 0x40826cc0,
      valid: true,
      via: 'page',
      access: 'rw',
      space: undefined,
    });
    expect(bridge.calls[0].args).toEqual({ addr: 0x40826cc0, supervisor: false });
  });

  it('walk keeps every step with its fields', async () => {
    bridge.reply('machine.cpu.mmu.walk', {
      phys: 0x408764,
      valid: true,
      via: 'page',
      access: 'rw',
      steps: [
        { step: 'bat', outcome: 'miss', name: 'dbat' },
        { step: 'segment', outcome: 'next', name: 'sr', index: 15, desc: 0xf, t: false },
        { step: 'pteg', outcome: 'hit', name: 'primary', addr: 0x4e01c0, slot: 0, phys: 0x408764 },
      ],
    });
    const w = await walkAddr(0xff808764, true);
    expect(w?.steps.map((s) => `${s.step}:${s.outcome}`)).toEqual([
      'bat:miss',
      'segment:next',
      'pteg:hit',
    ]);
    expect(w?.steps[1].fields).toEqual({ name: 'sr', index: 15, desc: 0xf, t: false });
    expect(w?.phys).toBe(0x408764);
  });

  // The core marks addresses and register images for hex display, and the
  // JSON encoding carries those as "0x..." strings.
  it('reads hex-flagged numbers, which arrive as "0x..." strings', async () => {
    bridge.reply('machine.cpu.mmu.translate', { phys: '0x40826cc0', valid: true, via: 'page' });
    expect((await translateAddr(0x40826cc0))?.phys).toBe(0x40826cc0);
    bridge.reply('machine.cpu.mmu.walk', {
      phys: '0x0',
      valid: true,
      via: 'segment',
      steps: [{ step: 'segment', outcome: 'hit', index: 0, desc: '0x87f00000', t: true }],
    });
    const w = await walkAddr(0);
    expect(w?.phys).toBe(0);
    expect(w?.steps[0].fields).toEqual({ index: 0, desc: 0x87f00000, t: true });
    bridge.reply('machine.cpu.mmu.map', [
      { start: '0x0', size: '0x100000000', phys: '0x0', via: 'identity', access: 'rw' },
    ]);
    expect((await mapRange(0, undefined))?.[0].size).toBe(2 ** 32);
    bridge.reply('machine.cpu.mmu.tc', '0x80f84500');
    const regs = await readMmuState('68030_pmmu');
    expect(regs.find((r) => r.name === 'tc')?.value).toBe(0x80f84500);
  });

  it('walk answers null on an error', async () => {
    bridge.reply('machine.cpu.mmu.walk', { error: 'cpu not initialised' });
    expect(await walkAddr(0)).toBeNull();
  });

  it('map sends the range and the limit, and reads the runs', async () => {
    bridge.reply('machine.cpu.mmu.map', [
      { start: 0, size: 0x800000, phys: 0, via: 'page', access: 'rw' },
      { start: 0xfe0000, size: 0x4000, phys: 0, via: 'segment', access: 'ro', space: 'rom' },
    ]);
    const runs = await mapRange(0, undefined, true);
    expect(bridge.calls[0].args).toEqual({ start: 0, supervisor: true, limit: MAP_LIMIT });
    expect(runs?.[1]).toEqual({
      start: 0xfe0000,
      size: 0x4000,
      phys: 0,
      via: 'segment',
      access: 'ro',
      space: 'rom',
    });
  });

  it('descriptor sends (addr, count, format) positionally', async () => {
    bridge.reply('machine.cpu.mmu.descriptor', [{ addr: 0x3000, desc: 0x19, type: 'page' }]);
    const d = await readDescriptors(0x3000, 4, 'long');
    expect(bridge.calls[0].args).toEqual([0x3000, 4, 'long']);
    expect(d).toEqual([{ addr: 0x3000, desc: 0x19, type: 'page' }]);
  });

  it('offers each kind its own descriptor formats', () => {
    expect(descriptorFormats('68030_pmmu')).toEqual(['short', 'long']);
    expect(descriptorFormats('68040')[0]).toBe('page');
    expect(descriptorFormats('ppc_604')).toEqual(['pte']);
    expect(descriptorFormats('lisa_segment')).toEqual([]);
  });
});
