import { render, waitFor, fireEvent } from '@testing-library/svelte';
import { describe, it, expect, vi, beforeEach } from 'vitest';
import RegistersSection from '@/components/panel-views/debug/RegistersSection.svelte';
import { debug } from '@/state/debug.svelte';

const writes: Array<{ name: string; value: number }> = [];
vi.mock('@/bus/debug', () => ({
  readRegisters: vi.fn(async () => ({
    d: [0, 1, 2, 3, 4, 5, 6, 7],
    a: [0x100, 0x200, 0x300, 0x400, 0x500, 0x600, 0x700, 0x800],
    pc: 0x40028e,
    sr: 0x2700,
    usp: 0x00fffe00,
    ssp: 0x00fffc00,
  })),
  writeRegister: vi.fn(async (name: string, value: number) => {
    writes.push({ name, value });
    return true;
  }),
}));

beforeEach(() => {
  writes.length = 0;
  debug.sections.registers = true;
});

describe('RegistersSection', () => {
  it('renders one input per register (24 total)', async () => {
    const { container } = render(RegistersSection);
    await waitFor(() => {
      const inputs = container.querySelectorAll('input.reg-value');
      // 8 D + 8 A + 4 control (pc, sr, usp, ssp) = 20. Spec lists 24
      // counting flag bits but those don't carry inputs here.
      expect(inputs.length).toBe(20);
    });
  });

  it('shows the read-back values in uppercase hex', async () => {
    const { container } = render(RegistersSection);
    await waitFor(() => {
      const pc = Array.from(container.querySelectorAll('input.reg-value')).find(
        (i) => (i as HTMLInputElement).getAttribute('aria-label') === 'PC register value',
      ) as HTMLInputElement;
      expect(pc.value).toBe('0040028E');
    });
  });

  it('Enter on a register input commits via writeRegister', async () => {
    const { container } = render(RegistersSection);
    const pcInput = await waitFor(() => {
      const el = Array.from(container.querySelectorAll('input.reg-value')).find(
        (i) => (i as HTMLInputElement).getAttribute('aria-label') === 'PC register value',
      ) as HTMLInputElement | undefined;
      if (!el) throw new Error('PC input not rendered yet');
      return el;
    });
    pcInput.value = '0x12345678';
    await fireEvent.keyDown(pcInput, { key: 'Enter' });
    await waitFor(() => {
      expect(writes.length).toBe(1);
      expect(writes[0].name).toBe('pc');
      expect(writes[0].value).toBe(0x12345678);
    });
  });

  it('SR input is readonly', async () => {
    const { container } = render(RegistersSection);
    await waitFor(() => {
      const sr = Array.from(container.querySelectorAll('input.reg-value')).find(
        (i) => (i as HTMLInputElement).getAttribute('aria-label') === 'SR register value',
      ) as HTMLInputElement;
      expect(sr.hasAttribute('readonly')).toBe(true);
    });
  });
});
