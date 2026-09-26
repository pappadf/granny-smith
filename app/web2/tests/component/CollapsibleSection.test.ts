import { render, fireEvent } from '@testing-library/svelte';
import { describe, it, expect, vi } from 'vitest';
import { createRawSnippet } from 'svelte';
import CollapsibleSection from '@/components/common/CollapsibleSection.svelte';

function rawSnippet(html: string) {
  return createRawSnippet(() => ({ render: () => html }));
}

describe('CollapsibleSection', () => {
  it('renders title + body when open', () => {
    const { container } = render(CollapsibleSection, {
      title: 'ROM',
      open: true,
      onToggle: () => undefined,
      children: rawSnippet('<div data-testid="body">hello</div>'),
    });
    expect(container.querySelector('.title')?.textContent).toBe('ROM');
    expect(container.querySelector('[data-testid=body]')).not.toBeNull();
  });

  it('hides body when collapsed', () => {
    const { container } = render(CollapsibleSection, {
      title: 'ROM',
      open: false,
      onToggle: () => undefined,
      children: rawSnippet('<div data-testid="body">hello</div>'),
    });
    expect(container.querySelector('[data-testid=body]')).toBeNull();
  });

  it('clicking the header calls onToggle', async () => {
    const onToggle = vi.fn();
    const { container } = render(CollapsibleSection, {
      title: 'ROM',
      open: false,
      onToggle,
      children: rawSnippet('<span/>'),
    });
    await fireEvent.click(container.querySelector('.header .toggle') as HTMLElement);
    expect(onToggle).toHaveBeenCalled();
  });

  // The header used to be role="button" with tabindex -1: no keyboard could
  // reach it, and the actions (a "+") were nested inside it.
  it('toggles through a real, focusable button that reports its state', () => {
    const { container } = render(CollapsibleSection, {
      title: 'ROM',
      open: true,
      onToggle: () => undefined,
      children: rawSnippet('<span/>'),
    });
    const toggle = container.querySelector('.header button.toggle') as HTMLButtonElement;
    expect(toggle).not.toBeNull();
    expect(toggle.getAttribute('aria-expanded')).toBe('true');
    expect(toggle.tabIndex).toBe(0);
    expect(container.querySelector('[role="button"]')).toBeNull();
  });

  it('shows the count when provided', () => {
    const { container } = render(CollapsibleSection, {
      title: 'ROM',
      open: true,
      onToggle: () => undefined,
      count: 3,
      children: rawSnippet('<span/>'),
    });
    expect(container.querySelector('.count')?.textContent).toBe('3');
  });
});
