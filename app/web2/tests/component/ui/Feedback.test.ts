import { render, fireEvent, waitFor } from '@testing-library/svelte';
import { describe, it, expect, vi, afterEach } from 'vitest';
import { createRawSnippet, tick } from 'svelte';
import Badge from '@/components/ui/Badge.svelte';
import ProgressBar from '@/components/ui/ProgressBar.svelte';
import StatusDot from '@/components/ui/StatusDot.svelte';
import DriveLight from '@/components/ui/DriveLight.svelte';
import Card from '@/components/ui/Card.svelte';
import Hero from '@/components/ui/Hero.svelte';
import Callout from '@/components/ui/Callout.svelte';
import Modal from '@/components/common/Modal.svelte';
import ContextMenu, { closeContextMenu } from '@/components/common/ContextMenu.svelte';
import { askText, askConfirm, answerText, answerConfirm, dialogs } from '@/state/dialogs.svelte';

// A child snippet of plain HTML.
const html = (h: string) => createRawSnippet(() => ({ render: () => h }));

describe('Badge', () => {
  it('carries its intent, variant and legacy class', () => {
    const { container } = render(Badge, {
      intent: 'success',
      class: 'tag tag-tt',
      children: html('<span>TT</span>'),
    });
    const b = container.querySelector('.gs-badge.tag-tt')!;
    expect(b.getAttribute('data-intent')).toBe('success');
    expect(b.getAttribute('data-variant')).toBe('pill');
  });
});

describe('ProgressBar', () => {
  it('reports its value and state', () => {
    const { container } = render(ProgressBar, { value: 0.5, state: 'active', label: 'ROM' });
    const bar = container.querySelector('[role=progressbar]')!;
    expect(bar.getAttribute('aria-valuenow')).toBe('50');
    expect(bar.getAttribute('data-state')).toBe('active');
    expect(bar.getAttribute('aria-label')).toBe('ROM');
  });

  it('is indeterminate without a value, and keeps the legacy aliases', () => {
    const { container } = render(ProgressBar, { state: 'active', label: 'HD' });
    const bar = container.querySelector('[role=progressbar]')!;
    expect(bar.hasAttribute('data-indeterminate')).toBe(true);
    expect(bar.classList.contains('indeterminate')).toBe(true);
    expect(bar.hasAttribute('aria-valuenow')).toBe(false);
    const { container: c2 } = render(ProgressBar, { value: 1, state: 'done', label: 'x' });
    expect(c2.querySelector('.gs-progress')!.classList.contains('done')).toBe(true);
  });

  it('a failed bar is never indeterminate', () => {
    const { container } = render(ProgressBar, { state: 'failed', label: 'x' });
    expect(container.querySelector('.gs-progress')!.hasAttribute('data-indeterminate')).toBe(false);
  });
});

describe('StatusDot and DriveLight', () => {
  it('the dot shows its state', () => {
    const { container } = render(StatusDot, { state: 'crashed' });
    expect(container.querySelector('.gs-status-dot')!.getAttribute('data-state')).toBe('crashed');
  });

  it('the drive light shows its activity and keeps the legacy hooks', () => {
    const { container } = render(DriveLight, { label: 'FD', title: 'Floppy', activity: 'write' });
    const d = container.querySelector('.sb-drive')!;
    expect(d.getAttribute('data-activity')).toBe('write');
    expect(d.classList.contains('active-write')).toBe(true);
    expect(d.querySelector('.drive-ico')!.textContent).toBe('FD');
  });
});

describe('Card, Hero and Callout', () => {
  it('a card renders its heading and content', () => {
    const { container } = render(Card, { heading: 'Start', children: html('<p>rows</p>') });
    expect(container.querySelector('.gs-card .card-heading')!.textContent).toBe('Start');
    expect(container.querySelector('.gs-card p')!.textContent).toBe('rows');
  });

  it('the hero renders the title and subtitle with their legacy classes', () => {
    const { container } = render(Hero, {
      title: 'Granny Smith',
      subtitle: 'Sub',
      titleClass: 'title',
    });
    expect(container.querySelector('h1.title')!.textContent).toBe('Granny Smith');
    expect(container.querySelector('.gs-hero__subtitle')!.textContent).toBe('Sub');
  });

  it('a callout carries its intent and edge', () => {
    const { container } = render(Callout, {
      intent: 'warning',
      edge: 'top',
      children: html('<b>x</b>'),
    });
    const c = container.querySelector('.gs-callout')!;
    expect(c.getAttribute('data-intent')).toBe('warning');
    expect(c.getAttribute('data-edge')).toBe('top');
  });
});

describe('Modal', () => {
  it('a popover has a close button and its own variant', async () => {
    const onClose = vi.fn();
    render(Modal, { open: true, title: 'Levels', variant: 'popover', closeButton: true, onClose });
    const card = document.querySelector('.modal-card')!;
    expect(card.getAttribute('data-variant')).toBe('popover');
    await fireEvent.click(document.querySelector('.gs-modal__close') as HTMLElement);
    expect(onClose).toHaveBeenCalled();
  });

  it('takes focus when it opens and keeps Tab inside', async () => {
    render(Modal, {
      open: true,
      title: 'Trap',
      children: html('<div><button id="a">a</button><button id="b">b</button></div>'),
    });
    await tick();
    const card = document.querySelector('.modal-card') as HTMLElement;
    await waitFor(() => expect(card.contains(document.activeElement)).toBe(true));
    const b = document.getElementById('b') as HTMLElement;
    b.focus();
    await fireEvent.keyDown(b, { key: 'Tab' });
    expect(document.activeElement?.id).toBe('a');
    await fireEvent.keyDown(document.activeElement as HTMLElement, { key: 'Tab', shiftKey: true });
    expect(document.activeElement?.id).toBe('b');
  });
});

describe('Menu', () => {
  afterEach(() => closeContextMenu());

  it('a checkable item is a menuitemcheckbox with a check mark', () => {
    const noop = () => undefined;
    const { container } = render(ContextMenu, {
      items: [
        { label: 'On', action: noop, checked: true },
        { label: 'Off', action: noop, checked: false, disabled: true },
      ],
      x: 0,
      y: 0,
      onClose: noop,
    });
    const items = container.querySelectorAll('[role=menuitemcheckbox]');
    expect(items).toHaveLength(2);
    expect(items[0].getAttribute('aria-checked')).toBe('true');
    expect(items[0].querySelector('.gs-menu__check use')).not.toBeNull();
    expect(items[1].getAttribute('aria-disabled')).toBe('true');
  });

  it('a disabled item does not run its action', async () => {
    const action = vi.fn();
    const { container } = render(ContextMenu, {
      items: [{ label: 'No', action, disabled: true }],
      x: 0,
      y: 0,
      onClose: () => undefined,
    });
    await fireEvent.click(container.querySelector('.item') as HTMLElement);
    expect(action).not.toHaveBeenCalled();
  });
});

describe('dialogs (in-app prompt and confirm)', () => {
  it('askText resolves with the answer, askConfirm with the choice', async () => {
    const text = askText({ title: 'Name', label: 'Name', initial: 'a' });
    expect(dialogs.current?.kind).toBe('text');
    answerText('b');
    await expect(text).resolves.toBe('b');
    const ok = askConfirm({ message: 'Sure?', danger: true });
    expect(dialogs.current?.kind).toBe('confirm');
    answerConfirm(true);
    await expect(ok).resolves.toBe(true);
    expect(dialogs.current).toBeNull();
  });

  it('a new question cancels the open one', async () => {
    const first = askText({ title: 'A', label: 'A' });
    const second = askConfirm({ message: 'B' });
    await expect(first).resolves.toBeNull();
    answerConfirm(false);
    await expect(second).resolves.toBe(false);
  });
});
