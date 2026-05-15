import { render, fireEvent } from '@testing-library/svelte';
import { describe, it, expect, beforeEach } from 'vitest';
import DisplayToolbar from '@/components/display/DisplayToolbar.svelte';
import { machine } from '@/state/machine.svelte';
import { layout, setPanelPos } from '@/state/layout.svelte';

beforeEach(() => {
  machine.status = 'no-machine';
  setPanelPos('bottom');
  layout.panelCollapsed = false;
});

describe('DisplayToolbar', () => {
  it('disables machine-dependent buttons when no machine is running', () => {
    const { container } = render(DisplayToolbar);
    // Execution / view / actions buttons are all disabled.
    const disabled = Array.from(container.querySelectorAll('button:disabled'));
    // 7 buttons should be disabled: run, shutdown, strict/live/fast,
    // zoom-out, zoom-in, save. Plus the zoom <input>.
    expect(disabled.length).toBeGreaterThanOrEqual(7);
  });

  it('keeps layout, theme, and fullscreen buttons enabled', () => {
    const { container } = render(DisplayToolbar);
    const layoutBtns = container.querySelectorAll('.layout-controls .tbtn');
    layoutBtns.forEach((btn) => {
      expect((btn as HTMLButtonElement).disabled).toBe(false);
    });
  });

  it('clicking a layout button switches panel position', async () => {
    const { container } = render(DisplayToolbar);
    const rightBtn = container.querySelector(
      '.layout-btn[title="Panel Right"]',
    ) as HTMLButtonElement;
    await fireEvent.click(rightBtn);
    expect(layout.panelPos).toBe('right');
  });

  it('clicking the active layout button collapses the panel', async () => {
    const { container } = render(DisplayToolbar);
    const bottomBtn = container.querySelector(
      '.layout-btn[title="Panel Bottom"]',
    ) as HTMLButtonElement;
    await fireEvent.click(bottomBtn);
    expect(layout.panelCollapsed).toBe(true);
  });
});
