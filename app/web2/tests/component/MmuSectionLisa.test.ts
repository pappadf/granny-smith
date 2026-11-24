import { render, fireEvent, waitFor } from '@testing-library/svelte';
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { bridge } from '../helpers/bridgeMock';

vi.mock('@/bus/emulator', async () => (await import('../helpers/bridgeMock')).emulatorModule());

const { default: MmuSection } = await import('@/components/panel-views/debug/MmuSection.svelte');
const { machine } = await import('@/state/machine.svelte');
const { debug } = await import('@/state/debug.svelte');
const { debugFrame } = await import('@/state/debugFrame.svelte');

// The MMU section on a Lisa, against the core's real answer shapes: the
// context selector in place of S/U, the four-context comparison, the
// segment table, and the State tab while the machine runs.

// Segment 1 ($20000) in each context, as a desktop with three tools open
// maps it: context 0 (the OS) has none.
const PHYS = [undefined, 0x3f400, 0x1c0a00, 0x5a800];
function translateReply(args: unknown): unknown {
  const ctx = (args as { context?: number }).context;
  const c = ctx ?? 2; // omitted: the CPU's (here, the latch context 2)
  if (c === 0) return { valid: false, via: 'segment' };
  return { phys: PHYS[c], valid: true, via: 'segment', access: 'ro', space: 'ram' };
}

function tab(container: HTMLElement, name: string): HTMLElement {
  return Array.from(container.querySelectorAll('.tab')).find(
    (e) => e.textContent?.trim() === name,
  ) as HTMLElement;
}

beforeEach(() => {
  bridge.reset();
  bridge.reply('machine.cpu.mmu.start', false);
  bridge.reply('machine.cpu.mmu.context', 2);
  bridge.reply('machine.cpu.mmu.translate', translateReply);
  bridge.reply('machine.cpu.mmu.walk', (args: unknown) => ({
    ...(translateReply(args) as object),
    steps: [],
  }));
  machine.mmuEnabled = true;
  machine.mmuKind = 'lisa_segment';
  machine.status = 'paused';
  debug.sections.mmu = true;
  debug.mmuSubtab = 'state';
  debug.mmuContext = 'current';
  debug.mmuCompare = false;
  debug.mmuTransAddr = 0x20000;
  debugFrame.current = null;
});

afterEach(() => {
  machine.status = 'no-machine';
  vi.useRealTimers();
});

describe('MmuSection on a Lisa', () => {
  it('offers the four contexts (and the CPU’s own) instead of S/U, marking the latch’s', async () => {
    const { container } = render(MmuSection);
    expect(container.querySelectorAll('.su-btn').length).toBe(0);
    await waitFor(() => {
      const labels = Array.from(container.querySelectorAll('.ctx-btn')).map((e) =>
        e.textContent?.trim(),
      );
      expect(labels).toEqual(['Now', '0 OS', '1', '2 •', '3']);
    });
  });

  it('Translate passes the chosen context to the core', async () => {
    debug.mmuSubtab = 'translate';
    const { container } = render(MmuSection);
    await waitFor(() => expect(bridge.paths()).toContain('machine.cpu.mmu.walk'));
    const walk = bridge.calls.filter((c) => c.path === 'machine.cpu.mmu.walk');
    // "Now" sends neither a context nor a privilege: the CPU's own choice.
    expect(walk[0].args).toEqual({ addr: 0x20000 });
    bridge.calls.length = 0;
    const one = Array.from(container.querySelectorAll('.ctx-btn')).find(
      (e) => e.textContent?.trim() === '1',
    ) as HTMLElement;
    await fireEvent.click(one);
    await waitFor(() =>
      expect(bridge.calls.some((c) => c.path === 'machine.cpu.mmu.walk')).toBe(true),
    );
    const again = bridge.calls.find((c) => c.path === 'machine.cpu.mmu.walk');
    expect(again?.args).toEqual({ addr: 0x20000, context: 1 });
    await waitFor(() => expect(container.textContent).toContain('0003F400'));
  });

  it('All four contexts lists one row per context', async () => {
    debug.mmuSubtab = 'translate';
    const { container } = render(MmuSection);
    const box = (await waitFor(() => {
      const b = container.querySelector('.compare input');
      expect(b).not.toBeNull();
      return b;
    })) as HTMLInputElement;
    await fireEvent.click(box);
    await waitFor(() => expect(container.querySelectorAll('.ctx-table tbody tr').length).toBe(4));
    const rows = Array.from(container.querySelectorAll('.ctx-table tbody tr')).map((r) =>
      Array.from(r.querySelectorAll('td')).map((td) => td.textContent?.trim()),
    );
    expect(rows[0]).toEqual(['0 (OS)', 'not mapped (faults)', '', '', '']);
    expect(rows[1]).toEqual(['1', 'valid', '$0003F400', 'RAM', 'read-only']);
    expect(rows[2][2]).toBe('$001C0A00');
    expect(rows[3][2]).toBe('$0005A800');
    const sent = bridge.calls
      .filter((c) => c.path === 'machine.cpu.mmu.translate')
      .map((c) => (c.args as { context?: number }).context);
    expect(sent).toEqual([0, 1, 2, 3]);
  });

  it('Descriptors shows each segment in words, from the chosen context', async () => {
    bridge.reply('machine.cpu.mmu.descriptor', (args: unknown) => {
      const [seg, , ctx] = args as number[];
      return [
        {
          segment: seg,
          context: ctx,
          base: seg * 0x20000,
          sor: 0x1fa,
          slr: 0x5f9,
          type: 'ram-ro',
          limit: 249,
          stack: false,
          ro: true,
          phys: 0x3f400,
          first_page: 0,
          pages: 7,
        },
        {
          segment: seg + 1,
          context: ctx,
          base: (seg + 1) * 0x20000,
          sor: 0,
          slr: 0xc00,
          type: 'invalid',
        },
        {
          segment: 123,
          context: ctx,
          base: 123 * 0x20000,
          sor: 0x100,
          slr: 0x6fe,
          type: 'ram-rw-stack',
          limit: 254,
          stack: true,
          ro: false,
          phys: 0x20000,
          first_page: 1,
          pages: 255,
        },
      ];
    });
    // From PC: the walk names the segment the PC is in.
    bridge.reply('machine.cpu.mmu.walk', {
      phys: 0x3f400,
      valid: true,
      via: 'segment',
      steps: [{ step: 'segment', outcome: 'hit', name: 'segment', index: 1, context: 1 }],
    });
    debug.mmuContext = '1';
    debug.mmuSubtab = 'descriptors';
    debugFrame.current = { pc: 0x20000 } as never;
    const { container } = render(MmuSection);
    await waitFor(() => expect(container.querySelectorAll('.seg-row').length).toBe(3));
    const desc = bridge.calls.find((c) => c.path === 'machine.cpu.mmu.descriptor');
    expect((desc?.args as number[])[2]).toBe(1);
    const cells = (i: number) =>
      Array.from(container.querySelectorAll('.seg-row')[i].querySelectorAll('td')).map((td) =>
        td.textContent?.trim(),
      );
    expect(cells(0)).toEqual([
      '1',
      '$020000',
      'read-only RAM',
      '7 pages (3.5 KB)',
      '$03F400',
      'SOR=$1FA SLR=$5F9',
    ]);
    expect(cells(1).slice(0, 4)).toEqual(['2', '$040000', 'not present', '']);
    // A stack's valid window starts at its boundary page.
    expect(cells(2).slice(1, 5)).toEqual([
      '$F60200',
      'stack (read-write RAM)',
      '255 pages (127.5 KB)',
      '$020200',
    ]);
    expect(container.textContent).toContain('Context 1');
  });

  it('keeps reading the State tab while the machine runs', async () => {
    vi.useFakeTimers();
    machine.status = 'running';
    let ctx = 1;
    bridge.reply('machine.cpu.mmu.context', () => ctx);
    const { container } = render(MmuSection);
    await vi.waitFor(() =>
      expect(container.querySelector('[data-reg="context"] .reg-hex')?.textContent).toBe('1'),
    );
    expect(container.textContent).toContain('live');
    ctx = 3;
    await vi.advanceTimersByTimeAsync(600);
    await vi.waitFor(() =>
      expect(container.querySelector('[data-reg="context"] .reg-hex')?.textContent).toBe('3'),
    );
  });

  it('asks for a pause on the tabs that walk tables', () => {
    machine.status = 'running';
    debug.mmuSubtab = 'map';
    const { container } = render(MmuSection);
    expect(container.textContent).toContain('Pause the machine');
    expect(tab(container, 'State')).toBeTruthy();
  });
});
