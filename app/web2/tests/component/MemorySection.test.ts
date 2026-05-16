import { render, waitFor, fireEvent } from '@testing-library/svelte';
import { describe, it, expect, vi, beforeEach } from 'vitest';
import MemorySection from '@/components/panel-views/debug/MemorySection.svelte';
import { debug } from '@/state/debug.svelte';
import { machine } from '@/state/machine.svelte';

const peekCalls: Array<{ mode: 'logical' | 'physical'; addr: number; count: number }> = [];

vi.mock('@/bus/debug', () => ({
  peekBytes: vi.fn(async (addr: number, count: number) => {
    peekCalls.push({ mode: 'logical', addr, count });
    return new Uint8Array(count).fill(0xab);
  }),
  peekPhysBytes: vi.fn(async (addr: number, count: number) => {
    peekCalls.push({ mode: 'physical', addr, count });
    return new Uint8Array(count).fill(0xcd);
  }),
}));

beforeEach(() => {
  peekCalls.length = 0;
  debug.sections.memory = true;
  debug.memoryAddress = 0x00000000;
  debug.memoryMode = 'logical';
  machine.mmuEnabled = true;
});

describe('MemorySection', () => {
  it('renders 8 rows of bytes after the initial read', async () => {
    const { container } = render(MemorySection);
    await waitFor(() => {
      expect(container.querySelectorAll('.mem-row').length).toBe(8);
    });
  });

  it('Mode toggle switches between peekBytes and peekPhysBytes', async () => {
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
});
