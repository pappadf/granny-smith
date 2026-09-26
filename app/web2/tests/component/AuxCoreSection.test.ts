import { render, waitFor, fireEvent } from '@testing-library/svelte';
import { describe, it, expect, vi, beforeEach } from 'vitest';
import AuxCoreSection from '@/components/panel-views/debug/AuxCoreSection.svelte';
import SectionsPane from '@/components/panel-views/debug/SectionsPane.svelte';
import { debug } from '@/state/debug.svelte';
import { machine } from '@/state/machine.svelte';

const frameCalls: Array<{ core: string }> = [];

vi.mock('@/bus/debug', async (importOriginal) => ({
  ...(await importOriginal<typeof import('@/bus/debug')>()),
  loadDebugFrame: vi.fn(async (_a: unknown, _c: unknown, _b: unknown, core: string) => {
    frameCalls.push({ core });
    return {
      arch: 'dsp3210',
      pc: 0x50030004,
      rawRegs: { r1: 0x12, r21: 0x50031000, pc: 0x50030004, ps: 0x40, emr: 0x8000, dauc: 0 },
      regs: null,
      rows: [
        { addr: 0x50030000, phys: 0x50030000, valid: true, mnem: 'r1 = r2 + 4', ops: '' },
        { addr: 0x50030004, phys: 0x50030004, valid: true, mnem: 'goto r18', ops: '' },
      ],
      fpu: { prefix: 'A', data: [{ hex: '40000000_81', val: '1' }], control: [] },
    };
  }),
}));
vi.mock('@/bus/emulator', async (importOriginal) => ({
  ...(await importOriginal<typeof import('@/bus/emulator')>()),
  gsEval: vi.fn(async (path: string) => (path === 'machine.dsp.state' ? 'running' : null)),
}));

const DSP = { name: 'dsp', arch: 'dsp3210', freq: 66666667 };

beforeEach(() => {
  frameCalls.length = 0;
  debug.auxOpen = {};
  machine.status = 'paused';
  machine.auxCpus = [];
});

describe('AuxCoreSection', () => {
  it("reads the core's own frame when opened, and renders it with the shared layout", async () => {
    const { container, getByText } = render(AuxCoreSection, { props: { cpu: DSP } });
    expect(frameCalls).toEqual([]); // closed: nothing fetched
    await fireEvent.click(container.querySelector('header.header .toggle') as HTMLElement);
    await waitFor(() => expect(container.querySelectorAll('.aux-row').length).toBe(2));
    expect(frameCalls.every((c) => c.core === 'dsp')).toBe(true);
    // The DSP3210 layout: 16-bit control registers at their width.
    expect(getByText('EMR').nextElementSibling?.textContent).toBe('8000');
    expect(getByText('R21').nextElementSibling?.textContent).toBe('50031000');
    // The row at the PC is marked; the state and accumulators show.
    expect(container.querySelector('.aux-row.pc')?.textContent).toContain('goto r18');
    expect(container.querySelector('.aux-state')?.textContent).toContain('running');
    expect(getByText('A0')).toBeTruthy();
  });

  it('asks to pause while the machine runs', async () => {
    machine.status = 'running';
    debug.auxOpen = { dsp: true };
    const { container } = render(AuxCoreSection, { props: { cpu: DSP } });
    expect(container.querySelector('.aux-hint')?.textContent).toContain('Pause');
    expect(frameCalls).toEqual([]);
  });
});

describe('SectionsPane', () => {
  // capabilities.aux_cpus used to be read by nothing (F-08).
  it('renders one section per auxiliary core, and none without', () => {
    const none = render(SectionsPane);
    expect(none.container.textContent).not.toContain('DSP (dsp3210)');
    none.unmount();
    machine.auxCpus = [DSP];
    const { container } = render(SectionsPane);
    expect(container.textContent).toContain('DSP (dsp3210)');
  });
});
