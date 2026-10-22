import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import {
  openContextMenu,
  closeContextMenu,
  type ContextMenuItem,
} from '@/components/common/ContextMenu.svelte';

function flushFrames(): Promise<void> {
  return new Promise((r) => requestAnimationFrame(() => r()));
}

beforeEach(() => {
  closeContextMenu();
});

afterEach(() => {
  closeContextMenu();
});

describe('ContextMenu', () => {
  it('renders one menu item per non-separator item', async () => {
    openContextMenu(
      [
        { label: 'Open', action: () => undefined },
        { sep: true },
        { label: 'Delete', action: () => undefined, danger: true },
      ],
      40,
      40,
    );
    await flushFrames();
    const items = document.querySelectorAll('.context-menu .item');
    expect(items.length).toBe(2);
    const seps = document.querySelectorAll('.context-menu .sep');
    expect(seps.length).toBe(1);
    const danger = document.querySelector('.context-menu .item.danger');
    expect(danger?.textContent?.trim()).toBe('Delete');
  });

  // S1018-51: a select's option marked data-badge (the Add-card list's
  // "Substitute ROM" / "ROM needed") shows it as a pill in the menu.
  it('draws an item badge as a pill beside its label', async () => {
    openContextMenu(
      [
        { label: 'Display Card 8•24', action: () => undefined, badge: 'Substitute ROM' },
        { label: 'Voodoo2', action: () => undefined },
      ],
      40,
      40,
    );
    await flushFrames();
    const items = document.querySelectorAll('.context-menu .item');
    expect(items[0].querySelector('.gs-badge')?.textContent).toBe('Substitute ROM');
    expect(items[1].querySelector('.gs-badge')).toBeNull();
  });

  it('clicking an item fires its action and dismisses the menu', async () => {
    const action = vi.fn();
    openContextMenu([{ label: 'Open', action }], 40, 40);
    await flushFrames();
    const item = document.querySelector('.context-menu .item') as HTMLElement;
    item.click();
    expect(action).toHaveBeenCalled();
    await flushFrames();
    expect(document.querySelector('.context-menu')).toBeNull();
  });

  it('Escape dismisses the menu', async () => {
    openContextMenu([{ label: 'Open', action: () => undefined }], 40, 40);
    await flushFrames();
    expect(document.querySelector('.context-menu')).not.toBeNull();
    document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape' }));
    await flushFrames();
    expect(document.querySelector('.context-menu')).toBeNull();
  });

  it('opening a second menu dismisses the first', async () => {
    openContextMenu([{ label: 'A', action: () => undefined }], 10, 10);
    await flushFrames();
    openContextMenu([{ label: 'B', action: () => undefined }], 20, 20);
    await flushFrames();
    const menus = document.querySelectorAll('.context-menu');
    expect(menus.length).toBe(1);
    expect(menus[0].textContent).toContain('B');
  });

  it('arrow keys cycle through items', async () => {
    const items: ContextMenuItem[] = [
      { label: 'One', action: () => undefined },
      { label: 'Two', action: () => undefined },
      { sep: true },
      { label: 'Three', action: () => undefined },
    ];
    openContextMenu(items, 40, 40);
    await flushFrames();
    document.dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowDown' }));
    document.dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowDown' }));
    document.dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowDown' }));
    await flushFrames();
    const highlighted = document.querySelector('.context-menu .item.highlight');
    // First ArrowDown lands on "One", second on "Two", third skips the
    // separator and lands on "Three".
    expect(highlighted?.textContent?.trim()).toBe('Three');
  });
});
