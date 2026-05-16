import { render, fireEvent, waitFor } from '@testing-library/svelte';
import { describe, it, expect, beforeEach } from 'vitest';
import MmuSection from '@/components/panel-views/debug/MmuSection.svelte';
import { machine } from '@/state/machine.svelte';
import { debug } from '@/state/debug.svelte';

beforeEach(() => {
  machine.mmuEnabled = true;
  debug.sections.mmu = true;
  debug.mmuSubtab = 'state';
  debug.mmuSupervisor = false;
});

describe('MmuSection', () => {
  it('is hidden when the machine has no MMU', () => {
    machine.mmuEnabled = false;
    const { container } = render(MmuSection);
    // The CollapsibleSection isn't rendered at all when MMU is off.
    expect(container.querySelector('.section')).toBeNull();
  });

  it('renders the 4 sub-tab buttons + the S/U toggle', async () => {
    const { container } = render(MmuSection);
    await waitFor(() => {
      const tabs = Array.from(container.querySelectorAll('.tab')).map((e) => e.textContent?.trim());
      expect(tabs).toEqual(['State', 'Translate', 'Map', 'Descriptors']);
    });
    const suBtns = container.querySelectorAll('.su-btn');
    expect(suBtns.length).toBe(2);
  });

  it('clicking a tab switches the active subtab', async () => {
    const { container } = render(MmuSection);
    const translateTab = Array.from(container.querySelectorAll('.tab')).find(
      (e) => e.textContent?.trim() === 'Translate',
    ) as HTMLElement;
    await fireEvent.click(translateTab);
    await waitFor(() => {
      expect(debug.mmuSubtab).toBe('translate');
    });
  });

  it('S/U toggle updates debug.mmuSupervisor', async () => {
    const { container } = render(MmuSection);
    const sBtn = Array.from(container.querySelectorAll('.su-btn')).find(
      (e) => e.textContent?.trim() === 'S',
    ) as HTMLElement;
    await fireEvent.click(sBtn);
    expect(debug.mmuSupervisor).toBe(true);
  });
});
