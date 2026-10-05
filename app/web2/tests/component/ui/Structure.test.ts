import { render, fireEvent } from '@testing-library/svelte';
import { describe, it, expect, vi } from 'vitest';
import { createRawSnippet } from 'svelte';
import Disclosure from '@/components/ui/Disclosure.svelte';
import TreeItem from '@/components/ui/TreeItem.svelte';
import ListRow from '@/components/ui/ListRow.svelte';
import SectionHeading from '@/components/ui/SectionHeading.svelte';
import Hint from '@/components/ui/Hint.svelte';
import Sash from '@/components/ui/Sash.svelte';
import Switch from '@/components/ui/Switch.svelte';
import Toolbar from '@/components/ui/Toolbar.svelte';
import Spinner from '@/components/ui/Spinner.svelte';
import Separator from '@/components/ui/Separator.svelte';
import ActivityDot from '@/components/ui/ActivityDot.svelte';

// A child snippet of plain HTML.
const html = (h: string) => createRawSnippet(() => ({ render: () => h }));

describe('Disclosure', () => {
  it('reports open, closed, leaf and loading as data-state', () => {
    const states = [
      [{ open: true }, 'open'],
      [{ open: false }, 'closed'],
      [{ open: false, hasChildren: false }, 'leaf'],
      [{ open: false, loading: true }, 'loading'],
    ] as const;
    for (const [props, want] of states) {
      const { container, unmount } = render(Disclosure, { ...props, class: 'twistie' });
      expect(container.querySelector('.gs-disclosure.twistie')!.getAttribute('data-state')).toBe(
        want,
      );
      unmount();
    }
  });

  it('is a button only with a click handler, keeping the legacy classes', async () => {
    const onclick = vi.fn();
    const { container } = render(Disclosure, { open: true, onclick, class: 'twistie' });
    const d = container.querySelector('.twistie.has.open') as HTMLElement;
    expect(d.getAttribute('role')).toBe('button');
    expect(d.getAttribute('aria-label')).toBe('Collapse');
    await fireEvent.click(d);
    expect(onclick).toHaveBeenCalled();
    const { container: c2 } = render(Disclosure, { open: false });
    expect(c2.querySelector('.gs-disclosure')!.getAttribute('aria-hidden')).toBe('true');
  });

  it('shows a spinner while loading', () => {
    const { container } = render(Disclosure, { open: false, loading: true });
    expect(container.querySelector('.gs-spinner')).not.toBeNull();
  });
});

describe('TreeItem', () => {
  it('renders the label, description, depth and legacy class', () => {
    const { container } = render(TreeItem, {
      depth: 2,
      label: 'hd',
      description: '1 MB',
      class: 'tree-row',
      role: 'treeitem',
    });
    const row = container.querySelector('.gs-tree-item.tree-row') as HTMLElement;
    expect(row.getAttribute('role')).toBe('treeitem');
    expect(row.style.getPropertyValue('--depth')).toBe('2');
    expect(row.querySelector('.label')!.textContent).toBe('hd');
    expect(row.querySelector('.desc')!.textContent).toBe('1 MB');
  });

  it('exposes selection, drag, filter, kind and variant', () => {
    const { container } = render(TreeItem, {
      depth: 0,
      label: 'x',
      selected: true,
      dragSource: true,
      kind: 'method',
      variant: 'category',
    });
    const row = container.querySelector('.gs-tree-item')!;
    expect(row.classList.contains('selected')).toBe(true);
    expect(row.hasAttribute('data-selected')).toBe(true);
    expect(row.getAttribute('data-state')).toBe('drag-source');
    expect(row.getAttribute('data-kind')).toBe('method');
    expect(row.classList.contains('gs-tree-item--category')).toBe(true);
    const { container: c2 } = render(TreeItem, { depth: 0, label: 'y', filter: 'dim' });
    expect(c2.querySelector('.gs-tree-item')!.getAttribute('data-state')).toBe('dim');
  });

  it('renders custom content and calls the disclosure handler', async () => {
    const onDisclosureClick = vi.fn();
    const { container } = render(TreeItem, {
      depth: 0,
      hasChildren: true,
      content: html('<span class="name">cpu</span>'),
      onDisclosureClick,
    });
    expect(container.querySelector('.name')!.textContent).toBe('cpu');
    await fireEvent.click(container.querySelector('.twistie') as HTMLElement);
    expect(onDisclosureClick).toHaveBeenCalled();
  });

  it('a placeholder row has no disclosure', () => {
    const { container } = render(TreeItem, { depth: 1, variant: 'placeholder', label: '(empty)' });
    expect(container.querySelector('.gs-tree-item--placeholder')!.textContent?.trim()).toBe(
      '(empty)',
    );
    expect(container.querySelector('.gs-disclosure')).toBeNull();
  });
});

describe('ListRow', () => {
  it('passes its role and handlers and marks the selection', async () => {
    const onclick = vi.fn();
    const { container } = render(ListRow, {
      class: 'image-row',
      role: 'row',
      selected: true,
      mono: true,
      density: 'compact',
      onclick,
      children: html('<span>a</span>'),
    });
    const row = container.querySelector('.gs-list-row.image-row') as HTMLElement;
    expect(row.getAttribute('role')).toBe('row');
    expect(row.getAttribute('data-density')).toBe('compact');
    expect(row.hasAttribute('data-selected')).toBe(true);
    expect(row.classList.contains('gs-list-row--mono')).toBe(true);
    await fireEvent.click(row);
    expect(onclick).toHaveBeenCalled();
  });
});

describe('SectionHeading and Hint', () => {
  it('a heading renders its level, or another element with `as`', () => {
    const { container } = render(SectionHeading, { level: 'h4', children: html('<b>Data</b>') });
    expect(container.querySelector('h4.gs-heading')!.textContent).toBe('Data');
    const { container: c2 } = render(SectionHeading, {
      as: 'span',
      class: 'label',
      children: html('<b>ROM</b>'),
    });
    expect(c2.querySelector('span.gs-heading.label')).not.toBeNull();
  });

  it('a hint carries its inset, and an error hint is an alert', () => {
    const { container } = render(Hint, {
      class: 'hint',
      inset: 'section',
      children: html('<b>none</b>'),
    });
    const p = container.querySelector('p.gs-hint.hint')!;
    expect(p.getAttribute('data-inset')).toBe('section');
    const { container: c2 } = render(Hint, { tone: 'error', children: html('<b>bad</b>') });
    expect(c2.querySelector('.gs-hint')!.getAttribute('role')).toBe('alert');
  });
});

describe('Sash', () => {
  it('is a separator with its orientation and a dragging state', () => {
    const { container } = render(Sash, {
      orientation: 'vertical',
      label: 'Resize',
      active: true,
      class: 'pane-sash',
    });
    const s = container.querySelector('.gs-sash.pane-sash')!;
    expect(s.getAttribute('role')).toBe('separator');
    expect(s.getAttribute('aria-orientation')).toBe('vertical');
    expect(s.getAttribute('data-state')).toBe('dragging');
    expect(s.classList.contains('active')).toBe(true);
  });
});

describe('Switch', () => {
  it('is a switch that reports the flipped value', async () => {
    const onchange = vi.fn();
    const { container } = render(Switch, {
      checked: false,
      label: 'Enabled',
      class: 'toggle',
      onchange,
    });
    const b = container.querySelector('button.gs-switch.toggle') as HTMLButtonElement;
    expect(b.getAttribute('role')).toBe('switch');
    expect(b.getAttribute('aria-checked')).toBe('false');
    await fireEvent.click(b);
    expect(onchange).toHaveBeenCalledWith(true);
  });
});

describe('Toolbar', () => {
  it('moves focus between its controls with the arrow keys, Home and End', async () => {
    const { container } = render(Toolbar, {
      label: 'Tools',
      children: html('<div><button>a</button><button disabled>b</button><button>c</button></div>'),
    });
    const bar = container.querySelector('[role=toolbar]') as HTMLElement;
    expect(bar.getAttribute('aria-label')).toBe('Tools');
    const [a, , c] = Array.from(bar.querySelectorAll('button'));
    a.focus();
    await fireEvent.keyDown(a, { key: 'ArrowRight' });
    expect(document.activeElement).toBe(c);
    await fireEvent.keyDown(c, { key: 'Home' });
    expect(document.activeElement).toBe(a);
    await fireEvent.keyDown(a, { key: 'End' });
    expect(document.activeElement).toBe(c);
  });
});

describe('Spinner', () => {
  it('is decorative without a label and a status with one', () => {
    const { container } = render(Spinner, {});
    expect(container.querySelector('.gs-spinner')!.getAttribute('aria-hidden')).toBe('true');
    const { container: c2 } = render(Spinner, { label: 'Loading', size: 'sm' });
    const s = c2.querySelector('.gs-spinner')!;
    expect(s.getAttribute('role')).toBe('status');
    expect(s.getAttribute('data-size')).toBe('sm');
  });
});

describe('Separator', () => {
  it('is a separator in either orientation, keeping its legacy class', () => {
    for (const orientation of ['vertical', 'horizontal'] as const) {
      const { container, unmount } = render(Separator, { orientation, class: 'sep' });
      const sep = container.querySelector('.gs-separator.sep')!;
      expect(sep.getAttribute('role')).toBe('separator');
      expect(sep.getAttribute('aria-orientation')).toBe(orientation);
      expect(sep.getAttribute('data-orientation')).toBe(orientation);
      unmount();
    }
  });
});

describe('ActivityDot', () => {
  it('is decorative', () => {
    const { container } = render(ActivityDot, { class: 'busy' });
    expect(container.querySelector('.gs-activity-dot.busy')!.getAttribute('aria-hidden')).toBe(
      'true',
    );
  });
});
