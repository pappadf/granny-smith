// The Disassembly pane's breakpoints (S1018-37): rows with one carry a
// marker, the context menu offers Add or Remove as the row has one or not,
// and an add reaches the Breakpoints list through debug.breakpointsGen.
import { render, fireEvent, waitFor } from '@testing-library/svelte';
import { describe, it, expect, vi, beforeEach } from 'vitest';
import type { Breakpoint } from '@/bus/debug';
import type { ContextMenuItem } from '@/components/common/ContextMenu.svelte';

let bpList: Breakpoint[] = [];
let menu: ContextMenuItem[] = [];

vi.mock('@/bus/debug', () => ({
  listBreakpoints: vi.fn(async () => bpList.slice()),
  addBreakpoint: vi.fn(async (addr: number) => {
    bpList.push({ id: bpList.length + 1, addr, enabled: true, hits: 0 });
    return true;
  }),
  removeBreakpointAt: vi.fn(async (addr: number) => {
    const before = bpList.length;
    bpList = bpList.filter((b) => b.addr !== addr);
    return bpList.length < before;
  }),
}));

vi.mock('@/components/common/ContextMenu.svelte', () => ({
  openContextMenu: (items: ContextMenuItem[]) => {
    menu = items;
  },
}));

const { default: DisassemblyPane } =
  await import('@/components/panel-views/debug/DisassemblyPane.svelte');
const { debugFrame } = await import('@/state/debugFrame.svelte');
const { debug } = await import('@/state/debug.svelte');
const { machine } = await import('@/state/machine.svelte');

const row = (addr: number) => ({ addr, phys: null, valid: true, mnem: 'NOP', ops: '' });

function labels(): string[] {
  return menu.filter((i) => 'label' in i).map((i) => (i as { label: string }).label);
}

beforeEach(() => {
  bpList = [];
  menu = [];
  machine.status = 'paused';
  machine.mmuEnabled = false;
  debugFrame.current = {
    arch: 'm68k',
    pc: 0x100,
    rawRegs: {},
    regs: null,
    rows: [row(0x100), row(0x102), row(0x104)],
  };
});

describe('DisassemblyPane breakpoints', () => {
  it('marks rows with a breakpoint and offers only the action that applies', async () => {
    bpList = [{ id: 1, addr: 0x102, enabled: true, hits: 0 }];
    const { container } = render(DisassemblyPane);
    await waitFor(() =>
      expect(container.querySelectorAll('.row')[1].getAttribute('data-state')).toBe('breakpoint'),
    );
    const rows = container.querySelectorAll('.row');
    expect(rows[2].getAttribute('data-state')).toBeNull();
    await fireEvent.contextMenu(rows[1]);
    expect(labels()).toContain('Remove breakpoint at $00000102');
    expect(labels().some((l) => l.startsWith('Add breakpoint'))).toBe(false);
    await fireEvent.contextMenu(rows[2]);
    expect(labels()).toContain('Add breakpoint at $00000104');
    expect(labels().some((l) => l.startsWith('Remove breakpoint'))).toBe(false);
  });

  it('an add marks the row and tells the other surfaces', async () => {
    const { container } = render(DisassemblyPane);
    const gen = debug.breakpointsGen;
    await fireEvent.contextMenu(container.querySelectorAll('.row')[2]);
    const add = menu.find(
      (i) => 'label' in i && i.label.startsWith('Add breakpoint'),
    ) as unknown as {
      action: () => Promise<void>;
    };
    await add.action();
    expect(debug.breakpointsGen).toBe(gen + 1);
    await waitFor(() =>
      expect(container.querySelectorAll('.row')[2].getAttribute('data-state')).toBe('breakpoint'),
    );
  });
});
