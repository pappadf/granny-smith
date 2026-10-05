import { render, waitFor, fireEvent } from '@testing-library/svelte';
import { describe, it, expect, vi, beforeEach } from 'vitest';
import RegistersSection from '@/components/panel-views/debug/RegistersSection.svelte';
import { debug } from '@/state/debug.svelte';
import { debugFrame } from '@/state/debugFrame.svelte';
import { machine } from '@/state/machine.svelte';
import type { DebugFrame } from '@/bus/debug';

const writes: Array<{ name: string; value: number }> = [];
vi.mock('@/bus/debug', () => ({
  writeRegister: vi.fn(async (name: string, value: number) => {
    writes.push({ name, value });
    return true;
  }),
}));

// A frame as the shared slice holds it (state/debugFrame.svelte.ts).
function frame(arch: string, rawRegs: Record<string, number>): DebugFrame {
  return { arch, pc: rawRegs.pc ?? 0, rawRegs, regs: null, rows: [] };
}

const M68K = frame('m68k', {
  ...Object.fromEntries(Array.from({ length: 8 }, (_, i) => [`d${i}`, i])),
  ...Object.fromEntries(Array.from({ length: 8 }, (_, i) => [`a${i}`, 0x100 * (i + 1)])),
  pc: 0x40028e,
  sr: 0x2700,
  usp: 0x00fffe00,
  ssp: 0x00fffc00,
});

function input(container: HTMLElement, name: string): HTMLInputElement | undefined {
  return Array.from(container.querySelectorAll('input.reg-value')).find(
    (i) => (i as HTMLInputElement).getAttribute('aria-label') === `${name} register value`,
  ) as HTMLInputElement | undefined;
}

beforeEach(() => {
  writes.length = 0;
  debug.sections.registers = true;
  machine.status = 'paused';
  debugFrame.current = M68K;
});

describe('RegistersSection', () => {
  it('renders the 68K register file: 8 D + 8 A + PC/SR/USP/SSP', async () => {
    const { container } = render(RegistersSection);
    await waitFor(() => expect(container.querySelectorAll('input.reg-value').length).toBe(20));
  });

  it('shows the read-back values in uppercase hex', async () => {
    const { container } = render(RegistersSection);
    await waitFor(() => expect(input(container, 'PC')?.value).toBe('0040028E'));
  });

  it('Enter on a register input commits via writeRegister', async () => {
    const { container } = render(RegistersSection);
    const pcInput = await waitFor(() => {
      const el = input(container, 'PC');
      if (!el) throw new Error('PC input not rendered yet');
      return el;
    });
    pcInput.value = '0x12345678';
    await fireEvent.keyDown(pcInput, { key: 'Enter' });
    await waitFor(() => expect(writes).toEqual([{ name: 'pc', value: 0x12345678 }]));
  });

  it('SR input is readonly', async () => {
    const { container } = render(RegistersSection);
    await waitFor(() => expect(input(container, 'SR')?.hasAttribute('readonly')).toBe(true));
  });

  // Before D4 a PowerPC machine showed nothing here (and, through the old
  // readRegisters path, a plausible 68K file of zeros).
  it('renders the PowerPC register file by its own names', async () => {
    debugFrame.current = frame('ppc', {
      ...Object.fromEntries(Array.from({ length: 32 }, (_, i) => [`r${i}`, i])),
      pc: 0xfff0345c,
      lr: 1,
      ctr: 2,
      cr: 3,
      xer: 4,
      msr: 0x3030,
      srr0: 5,
      srr1: 6,
    });
    const { container } = render(RegistersSection);
    await waitFor(() => expect(container.querySelectorAll('input.reg-value').length).toBe(40));
    expect(input(container, 'R31')?.value).toBe('0000001F');
    expect(input(container, 'MSR')?.value).toBe('00003030');
    expect(input(container, 'D0')).toBeUndefined();
    expect(container.textContent).toContain('General');
  });

  it('renders any architecture without a layout as one generic grid', async () => {
    debugFrame.current = frame('dsp3210', { pc: 0x10, r1: 7 });
    const { container } = render(RegistersSection);
    await waitFor(() => expect(container.querySelectorAll('input.reg-value').length).toBe(2));
    expect(container.textContent).toContain('Registers');
  });
});
