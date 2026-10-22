import { render, fireEvent, waitFor } from '@testing-library/svelte';
import { describe, it, expect, beforeEach, vi } from 'vitest';
import DisplayToolbar from '@/components/display/DisplayToolbar.svelte';
import { machine } from '@/state/machine.svelte';
import { layout, setPanelPos } from '@/state/layout.svelte';
import { appearance } from '@/state/appearance.svelte';
import { closeContextMenu } from '@/components/common/ContextMenu.svelte';

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

  it('the appearance menu lists the skins, and picks one', async () => {
    appearance.skin = 'workbench';
    const { container } = render(DisplayToolbar);
    await fireEvent.click(container.querySelector('.appearance-menu') as HTMLElement);
    const items = Array.from(document.querySelectorAll('.context-menu .item'));
    const labels = items.map((i) => i.textContent?.trim());
    expect(labels).toEqual([
      'Midnight',
      'Starlight',
      'Platinum',
      'Aqua',
      'Workbench',
      'Workbench Light',
    ]);
    await fireEvent.click(items[1] as HTMLElement);
    expect(appearance.skin).toBe('starlight');
    appearance.skin = 'workbench';
    closeContextMenu();
  });

  it('the screen-mode menu offers the three modes, current one checked', async () => {
    layout.fullscreen = false;
    const { container } = render(DisplayToolbar);
    await fireEvent.click(container.querySelector('.fullscreen-menu') as HTMLElement);
    const items = Array.from(document.querySelectorAll('.context-menu .item'));
    expect(items.map((i) => i.textContent?.trim())).toEqual([
      'Normal',
      'Full Screen',
      'Full Screen, Hide Toolbar and Status Bar',
    ]);
    closeContextMenu();
  });

  // S1018-69: a bar too narrow for its tools moves whole groups, right to
  // left, into the "More" menu.  The layout is faked: a 400 px bar whose
  // last group ends where the visible groups' widths add up to.
  it('a narrow bar moves the groups that do not fit into the More menu', async () => {
    const real = Element.prototype.getBoundingClientRect;
    const spy = vi.spyOn(Element.prototype, 'getBoundingClientRect').mockImplementation(function (
      this: Element,
    ) {
      if (this.classList.contains('gs-toolbar')) return { right: 400 } as DOMRect;
      if (this.classList.contains('layout-controls')) {
        const bar = this.closest('.gs-toolbar')!;
        const has = (sel: string) => (bar.querySelector(sel) ? 1 : 0);
        const right =
          150 +
          150 * has('.scheduler') +
          120 * has('.tg.view') +
          60 * has('.tg.actions') +
          200 * has('.appearance-menu');
        return { right } as DOMRect;
      }
      return real.call(this);
    });
    try {
      const { container } = render(DisplayToolbar);
      await waitFor(() => expect(container.querySelector('.tg.view')).toBeNull());
      await new Promise((r) => setTimeout(r, 0));
      const more = container.querySelector('.overflow-menu') as HTMLElement;
      expect(more).not.toBeNull();
      expect(container.querySelector('.scheduler')).not.toBeNull();
      expect(container.querySelector('.tg.actions')).toBeNull();
      expect(container.querySelector('.appearance-menu')).toBeNull();
      await fireEvent.click(more);
      const items = Array.from(document.querySelectorAll('.context-menu .item'));
      const labels = items.map((i) => i.textContent?.trim());
      expect(labels).toEqual(
        expect.arrayContaining(['Zoom Out', 'Zoom In', 'Save State', 'Platinum', 'Panel Right']),
      );
      expect(labels).not.toContain('Max');
      await fireEvent.click(items[labels.indexOf('Panel Right')] as HTMLElement);
      expect(layout.panelPos).toBe('right');
      closeContextMenu();
    } finally {
      spy.mockRestore();
    }
  });

  it('a bar wide enough shows every group and no More button', async () => {
    const { container } = render(DisplayToolbar);
    await new Promise((r) => setTimeout(r, 0));
    expect(container.querySelector('.overflow-menu')).toBeNull();
    expect(container.querySelector('.appearance-menu')).not.toBeNull();
  });
});
