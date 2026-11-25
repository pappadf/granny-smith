import { render, waitFor, fireEvent } from '@testing-library/svelte';
import { describe, it, expect, vi, beforeEach } from 'vitest';
import MemorySection from '@/components/panel-views/debug/MemorySection.svelte';
import { debug } from '@/state/debug.svelte';
import { machine } from '@/state/machine.svelte';
import { toasts, _resetForTests } from '@/state/toasts.svelte';

const peekCalls: Array<{
  mode: 'logical' | 'physical';
  addr: number;
  count: number;
  context?: number;
}> = [];

vi.mock('@/bus/debug', () => ({
  peekBytes: vi.fn(
    async (addr: number, count: number, space: 'logical' | 'physical', context?: number) => {
      peekCalls.push({ mode: space, addr, count, context });
      return new Uint8Array(count).fill(space === 'physical' ? 0xcd : 0xab);
    },
  ),
}));
// The row labels ask the core for translations; answer "unknown".
vi.mock('@/bus/mmu', () => ({
  translateMany: vi.fn(async () => ({})),
  addrLabel: (addr: number) => `$${addr.toString(16)}`,
}));

beforeEach(() => {
  peekCalls.length = 0;
  debug.sections.memory = true;
  debug.memoryAddress = 0x00000000;
  debug.memoryMode = 'logical';
  debug.mmuContext = 'current';
  machine.mmuEnabled = true;
  machine.mmuKind = '68030_pmmu';
});

describe('MemorySection', () => {
  it('an address that does not parse says so (S1018-84)', async () => {
    _resetForTests();
    const { container } = render(MemorySection);
    const input = container.querySelector('input[aria-label="Memory address"]') as HTMLInputElement;
    await fireEvent.input(input, { target: { value: 'garbage!' } });
    await fireEvent.keyDown(input, { key: 'Enter' });
    expect(toasts.active.map((t) => t.msg)).toEqual(['Invalid memory address']);
    expect(debug.memoryAddress).toBe(0);
  });

  it('renders 8 rows of bytes after the initial read', async () => {
    const { container } = render(MemorySection);
    await waitFor(() => {
      expect(container.querySelectorAll('.mem-row').length).toBe(8);
    });
  });

  it('the Mode toggle passes the space to the core', async () => {
    const { container } = render(MemorySection);
    await waitFor(() => {
      expect(peekCalls.length).toBeGreaterThan(0);
    });
    peekCalls.length = 0;
    const physBtn = Array.from(container.querySelectorAll('.mem-mode-btn')).find(
      (e) => e.textContent?.trim() === 'Physical',
    ) as HTMLElement;
    await fireEvent.click(physBtn);
    await waitFor(() => {
      expect(peekCalls.some((c) => c.mode === 'physical')).toBe(true);
    });
  });

  it('Mode toggle is hidden when machine.mmuEnabled is false', () => {
    machine.mmuEnabled = false;
    const { container } = render(MemorySection);
    expect(container.querySelector('.mem-mode')).toBeNull();
  });

  // The Lisa: Physical is main memory by physical address, and a logical
  // read names one of the four MMU contexts (none: the CPU's own).
  it('offers Physical mode and a context choice on the Lisa', async () => {
    machine.mmuKind = 'lisa_segment';
    const { container } = render(MemorySection);
    expect(container.querySelector('.mem-mode')).not.toBeNull();
    await waitFor(() => expect(peekCalls.length).toBeGreaterThan(0));
    expect(peekCalls.every((c) => c.context === undefined)).toBe(true);
    peekCalls.length = 0;
    const ctx2 = Array.from(container.querySelectorAll('.mem-ctx-btn')).find(
      (e) => e.textContent?.trim() === '2',
    ) as HTMLElement;
    await fireEvent.click(ctx2);
    expect(debug.mmuContext).toBe('2');
    await waitFor(() => {
      expect(peekCalls.some((c) => c.mode === 'logical' && c.context === 2)).toBe(true);
    });
  });

  it('offers no context choice on other machines', () => {
    const { container } = render(MemorySection);
    expect(container.querySelector('.mem-ctx')).toBeNull();
  });
});
