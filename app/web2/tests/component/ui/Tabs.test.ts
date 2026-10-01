import { render, fireEvent } from '@testing-library/svelte';
import { describe, it, expect, vi } from 'vitest';
import Tabs from '@/components/ui/Tabs.svelte';

describe('Tabs', () => {
  it('renders one button per tab and highlights the active one', () => {
    const { container } = render(Tabs, {
      tabClass: 'tab',
      tabs: [
        { key: 'a', label: 'Alpha' },
        { key: 'b', label: 'Beta' },
        { key: 'c', label: 'Gamma' },
      ],
      active: 'b',
      onSelect: () => undefined,
    });
    const tabs = container.querySelectorAll('.tab');
    expect(tabs.length).toBe(3);
    expect(tabs[1].getAttribute('aria-selected')).toBe('true');
    expect(tabs[0].getAttribute('aria-selected')).toBe('false');
  });

  it('clicking a tab calls onSelect with the key', async () => {
    const onSelect = vi.fn();
    const { container } = render(Tabs, {
      tabClass: 'tab',
      tabs: [
        { key: 'x', label: 'X' },
        { key: 'y', label: 'Y' },
      ],
      active: 'x',
      onSelect,
    });
    const yTab = Array.from(container.querySelectorAll('.tab')).find(
      (e) => e.textContent?.trim() === 'Y',
    ) as HTMLElement;
    await fireEvent.click(yTab);
    expect(onSelect).toHaveBeenCalledWith('y');
  });

  it('aria-selected matches the active key', () => {
    const { container } = render(Tabs, {
      tabClass: 'tab',
      tabs: [
        { key: 'a', label: 'A' },
        { key: 'b', label: 'B' },
      ],
      active: 'a',
      onSelect: () => undefined,
    });
    const tabs = container.querySelectorAll('.tab');
    expect(tabs[0].getAttribute('aria-selected')).toBe('true');
    expect(tabs[1].getAttribute('aria-selected')).toBe('false');
  });

  it('the sub variant moves the selection with the arrow keys', async () => {
    const onSelect = vi.fn();
    const { container } = render(Tabs, {
      tabs: [
        { key: 'a', label: 'A' },
        { key: 'b', label: 'B' },
      ],
      active: 'a',
      onSelect,
    });
    const strip = container.querySelector('[role=tablist]') as HTMLElement;
    await fireEvent.keyDown(strip, { key: 'ArrowRight' });
    expect(onSelect).toHaveBeenCalledWith('b');
    expect(container.querySelectorAll('[tabindex="0"]').length).toBe(1);
  });

  it('the panel variant keeps every tab tabbable and carries data-tab', () => {
    const { container } = render(Tabs, {
      variant: 'panel',
      tabClass: 'ptab',
      tabs: [
        { key: 'a', label: 'A' },
        { key: 'b', label: 'B' },
      ],
      active: 'b',
      onSelect: () => undefined,
    });
    const tabs = container.querySelectorAll('.ptab');
    expect(tabs[1].getAttribute('data-tab')).toBe('b');
    expect(container.querySelectorAll('[tabindex="-1"]').length).toBe(0);
  });
});
