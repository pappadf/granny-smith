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
    await fireEvent.click(container.querySelector('.header') as HTMLElement);
    expect(onToggle).toHaveBeenCalled();
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
