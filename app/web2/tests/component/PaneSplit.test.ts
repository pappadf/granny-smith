import { render } from '@testing-library/svelte';
import { describe, it, expect } from 'vitest';
import { mount, unmount, createRawSnippet } from 'svelte';
import PaneSplit from '@/components/common/PaneSplit.svelte';

function makeSnippet(label: string) {
  return createRawSnippet(() => ({
    render: () => `<div data-testid="${label}">${label}</div>`,
  }));
}

describe('PaneSplit', () => {
  it('renders both panes with a sash separator', () => {
    const { container } = render(PaneSplit, {
      orientation: 'horizontal',
      paneA: makeSnippet('a'),
      paneB: makeSnippet('b'),
    });
    expect(container.querySelector('[data-testid=a]')).not.toBeNull();
    expect(container.querySelector('[data-testid=b]')).not.toBeNull();
    expect(container.querySelector('.pane-sash')).not.toBeNull();
  });

  it('applies the vertical class when orientation is vertical', () => {
    const { container } = render(PaneSplit, {
      orientation: 'vertical',
      paneA: makeSnippet('top'),
      paneB: makeSnippet('bot'),
    });
    const root = container.querySelector('.pane-split');
    expect(root?.classList.contains('vertical')).toBe(true);
  });

  it('defaults to a 60% split for pane A when defaultSizePct is omitted', () => {
    const { container } = render(PaneSplit, {
      orientation: 'horizontal',
      paneA: makeSnippet('a'),
      paneB: makeSnippet('b'),
    });
    const paneA = container.querySelector('.pane-a') as HTMLElement;
    expect(paneA.style.flexBasis).toBe('60%');
  });

  it('honors defaultSizePct prop', () => {
    const { container } = render(PaneSplit, {
      orientation: 'horizontal',
      defaultSizePct: 30,
      paneA: makeSnippet('a'),
      paneB: makeSnippet('b'),
    });
    const paneA = container.querySelector('.pane-a') as HTMLElement;
    expect(paneA.style.flexBasis).toBe('30%');
  });

  it('the sash has role=separator with matching aria-orientation', () => {
    const { container } = render(PaneSplit, {
      orientation: 'horizontal',
      paneA: makeSnippet('a'),
      paneB: makeSnippet('b'),
    });
    const sash = container.querySelector('.pane-sash') as HTMLElement;
    expect(sash.getAttribute('role')).toBe('separator');
    // horizontal split → vertical sash
    expect(sash.getAttribute('aria-orientation')).toBe('vertical');
  });

  it('manual mount + unmount cleans up cleanly', () => {
    const target = document.createElement('div');
    document.body.appendChild(target);
    const inst = mount(PaneSplit, {
      target,
      props: {
        orientation: 'horizontal',
        paneA: makeSnippet('a'),
        paneB: makeSnippet('b'),
      },
    });
    expect(target.querySelector('.pane-split')).not.toBeNull();
    unmount(inst);
    document.body.removeChild(target);
  });
});
