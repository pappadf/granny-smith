import { render, fireEvent } from '@testing-library/svelte';
import { describe, it, expect, beforeEach } from 'vitest';
import WorkbenchSash from '@/components/workbench/WorkbenchSash.svelte';
import { layout, setPanelPos, resetPanelSizes } from '@/state/layout.svelte';

beforeEach(() => {
  setPanelPos('bottom');
  resetPanelSizes();
  layout.panelCollapsed = false;
});

describe('WorkbenchSash', () => {
  it('renders with role=separator and correct orientation', () => {
    const { container } = render(WorkbenchSash);
    const sash = container.querySelector('.workbench-sash');
    expect(sash).not.toBeNull();
    expect(sash?.getAttribute('role')).toBe('separator');
    expect(sash?.getAttribute('aria-orientation')).toBe('horizontal');
  });

  it('aria-orientation flips to vertical when panel is on the left', () => {
    setPanelPos('left');
    const { container } = render(WorkbenchSash);
    const sash = container.querySelector('.workbench-sash');
    expect(sash?.getAttribute('aria-orientation')).toBe('vertical');
  });

  it('double-click resets sizes to defaults', async () => {
    // Mutate one size to a non-default value.
    layout.panelSize.bottom = 500;
    const { container } = render(WorkbenchSash);
    const sash = container.querySelector('.workbench-sash') as HTMLElement;
    await fireEvent.dblClick(sash);
    expect(layout.panelSize.bottom).toBe(280);
    expect(layout.panelSize.left).toBe(340);
    expect(layout.panelSize.right).toBe(340);
  });
});
