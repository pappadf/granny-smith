import { render, fireEvent } from '@testing-library/svelte';
import { describe, it, expect, vi } from 'vitest';
import TabStrip from '@/components/common/TabStrip.svelte';

describe('TabStrip', () => {
  it('renders one button per tab and highlights the active one', () => {
    const { container } = render(TabStrip, {
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
    expect(tabs[1].classList.contains('active')).toBe(true);
    expect(tabs[0].classList.contains('active')).toBe(false);
  });

  it('clicking a tab calls onSelect with the key', async () => {
    const onSelect = vi.fn();
    const { container } = render(TabStrip, {
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
    const { container } = render(TabStrip, {
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
});
