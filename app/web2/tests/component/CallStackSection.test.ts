// The Call Stack walk stops at a link that cannot be the caller's frame
// (S1018-85): one below the current frame, or an odd one.
import { render, waitFor } from '@testing-library/svelte';
import { describe, it, expect, vi, beforeEach } from 'vitest';

let mem: Record<number, number> = {};

vi.mock('@/bus/debug', () => ({
  peekLogicalL: vi.fn(async (addr: number) => mem[addr] ?? 0),
}));
vi.mock('@/bus/mmu', () => ({
  translateMany: vi.fn(async () => ({})),
  addrLabel: (addr: number) => `$${addr.toString(16)}`,
}));

const { default: CallStackSection } = await import(
  '@/components/panel-views/debug/CallStackSection.svelte'
);
const { debug } = await import('@/state/debug.svelte');
const { debugFrame } = await import('@/state/debugFrame.svelte');
const { machine } = await import('@/state/machine.svelte');

function frameAt(a6: number): void {
  debugFrame.current = { arch: 'm68k', pc: 0x400000, rawRegs: { a6 }, regs: null, rows: [] };
}

const fps = (c: HTMLElement) =>
  Array.from(c.querySelectorAll('.frame-row .frame')).map((e) => e.textContent);

beforeEach(() => {
  debug.sections.callstack = true;
  machine.status = 'paused';
  machine.mmuEnabled = false;
});

describe('CallStackSection', () => {
  it('stops where the link goes down the stack', async () => {
    // Two real frames, then a link back below them.
    mem = { 0x1000: 0x1100, 0x1004: 0x400100, 0x1100: 0x01e4, 0x1104: 0x400200 };
    mem[0x01e4] = 0x2000;
    mem[0x01e8] = 0x400300;
    frameAt(0x1000);
    const { container } = render(CallStackSection);
    await waitFor(() => expect(fps(container)).toEqual(['fp=$00001000', 'fp=$00001100']));
  });

  it('stops at an odd link or an odd return address', async () => {
    mem = { 0x1000: 0x1101, 0x1004: 0x400100, 0x1101: 0x2000, 0x1105: 0x400200 };
    frameAt(0x1000);
    const { container } = render(CallStackSection);
    await waitFor(() => expect(fps(container)).toEqual(['fp=$00001000']));
    mem = { 0x1000: 0x1100, 0x1004: 0x400100, 0x1100: 0x2000, 0x1104: 0x400201 };
    frameAt(0x1000);
    await waitFor(() => expect(fps(container)).toEqual(['fp=$00001000']));
  });
});
