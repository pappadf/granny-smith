import { render, fireEvent, waitFor } from '@testing-library/svelte';
import { describe, it, expect, vi, beforeEach } from 'vitest';
import BreakpointsSection from '@/components/panel-views/debug/BreakpointsSection.svelte';
import { debug } from '@/state/debug.svelte';
import type { Breakpoint } from '@/bus/debug';

let bpList: Breakpoint[] = [];
const adds: Array<{ addr: number; cond?: string }> = [];
const removes: number[] = [];

vi.mock('@/bus/debug', () => ({
  listBreakpoints: vi.fn(async () => bpList.slice()),
  addBreakpoint: vi.fn(async (addr: number, cond?: string) => {
    adds.push({ addr, cond });
    bpList.push({ id: bpList.length + 1, addr, enabled: true, condition: cond, hits: 0 });
    return true;
  }),
  removeBreakpoint: vi.fn(async (addr: number) => {
    removes.push(addr);
    bpList = bpList.filter((b) => b.addr !== addr);
    return true;
  }),
}));

beforeEach(() => {
  bpList = [];
  adds.length = 0;
  removes.length = 0;
  debug.sections.breakpoints = true;
});

describe('BreakpointsSection', () => {
  it('shows the empty hint when no breakpoints exist', async () => {
    const { container } = render(BreakpointsSection);
    await waitFor(() => {
      expect(container.querySelector('.hint')?.textContent ?? '').toMatch(/no breakpoints/i);
    });
  });

  it('clicking + reveals the add row; Enter on address commits via addBreakpoint', async () => {
    const { container } = render(BreakpointsSection);
    const addBtn = container.querySelector('.add-btn') as HTMLElement;
    await fireEvent.click(addBtn);
    await waitFor(() => {
      expect(container.querySelector('.add-row')).not.toBeNull();
    });
    const addrInput = container.querySelector('input.add-addr') as HTMLInputElement;
    addrInput.value = '0x400e';
    await fireEvent.input(addrInput);
    await fireEvent.keyDown(addrInput, { key: 'Enter' });
    await waitFor(() => {
      expect(adds.length).toBe(1);
      expect(adds[0].addr).toBe(0x400e);
    });
  });

  it('renders one row per breakpoint in the list', async () => {
    bpList = [
      { id: 1, addr: 0x400000, enabled: true, hits: 0 },
      { id: 2, addr: 0x400e00, enabled: false, hits: 3, condition: 'd0 == 1' },
    ];
    const { container } = render(BreakpointsSection);
    await waitFor(() => {
      const rows = container.querySelectorAll('.bp-row');
      expect(rows.length).toBe(2);
    });
    const txt = container.textContent ?? '';
    expect(txt).toContain('3×');
    expect(txt).toContain('d0 == 1');
  });
});
