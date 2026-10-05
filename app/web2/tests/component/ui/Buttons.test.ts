import { render, fireEvent } from '@testing-library/svelte';
import { describe, it, expect, vi } from 'vitest';
import { createRawSnippet } from 'svelte';
import Button from '@/components/ui/Button.svelte';
import IconButton from '@/components/ui/IconButton.svelte';
import SegmentedControl from '@/components/ui/SegmentedControl.svelte';
import ToggleChip from '@/components/ui/ToggleChip.svelte';
import ActionRow from '@/components/ui/ActionRow.svelte';

// A text child for components that take one.
const text = (t: string) => createRawSnippet(() => ({ render: () => `<span>${t}</span>` }));

describe('Button', () => {
  it('renders a button with its variant, size and legacy class', () => {
    const { container } = render(Button, {
      variant: 'primary',
      size: 'lg',
      class: 'btn-primary',
      children: text('Go'),
    });
    const b = container.querySelector('button.gs-button.btn-primary')!;
    expect(b.getAttribute('data-variant')).toBe('primary');
    expect(b.getAttribute('data-size')).toBe('lg');
    expect(b.getAttribute('type')).toBe('button');
    expect(b.textContent).toContain('Go');
  });

  it('is a link with href', () => {
    const { container } = render(Button, { href: '#doc', download: 'a.pdf', children: text('Dl') });
    const a = container.querySelector('a.gs-button')!;
    expect(a.getAttribute('href')).toBe('#doc');
    expect(a.getAttribute('download')).toBe('a.pdf');
  });

  it('busy sets aria-busy and shows a spinner', () => {
    const { container } = render(Button, { busy: true, children: text('Save') });
    expect(container.querySelector('button')!.getAttribute('aria-busy')).toBe('true');
    expect(container.querySelector('.gs-button__spinner')).not.toBeNull();
  });

  it('forwards clicks and disabled', async () => {
    const onclick = vi.fn();
    const { container } = render(Button, { onclick, children: text('x') });
    await fireEvent.click(container.querySelector('button')!);
    expect(onclick).toHaveBeenCalled();
    const off = render(Button, { disabled: true, children: text('y') });
    expect(off.container.querySelector('button')!.disabled).toBe(true);
  });
});

describe('IconButton', () => {
  it('names itself by its label, also as the tooltip', () => {
    const { container } = render(IconButton, { icon: 'play', label: 'Run' });
    const b = container.querySelector('button')!;
    expect(b.getAttribute('aria-label')).toBe('Run');
    expect(b.getAttribute('title')).toBe('Run');
  });

  it('pressed maps to aria-pressed; live to data-state', () => {
    const { container } = render(IconButton, {
      icon: 'camera',
      label: 'Camera',
      pressed: true,
      live: true,
    });
    const b = container.querySelector('button')!;
    expect(b.getAttribute('aria-pressed')).toBe('true');
    expect(b.getAttribute('data-state')).toBe('live');
  });
});

describe('SegmentedControl', () => {
  it('marks the selected option pressed and reports a pick', async () => {
    const onChange = vi.fn();
    const { container } = render(SegmentedControl, {
      label: 'Mode',
      value: 'b',
      onChange,
      optionClass: 'sch-btn',
      options: [
        { value: 'a', label: 'A' },
        { value: 'b', label: 'B' },
      ],
    });
    expect(container.querySelector('[role=group]')!.getAttribute('aria-label')).toBe('Mode');
    const opts = container.querySelectorAll('button.sch-btn');
    expect(opts[0].getAttribute('aria-pressed')).toBe('false');
    expect(opts[1].getAttribute('aria-pressed')).toBe('true');
    expect(opts[1].classList.contains('active')).toBe(true);
    await fireEvent.click(opts[0]);
    expect(onChange).toHaveBeenCalledWith('a');
  });
});

describe('ToggleChip', () => {
  it('is a toggle button', async () => {
    const onToggle = vi.fn();
    const { container } = render(ToggleChip, { pressed: true, label: 'Caps Lock', onToggle });
    const b = container.querySelector('button')!;
    expect(b.getAttribute('aria-pressed')).toBe('true');
    await fireEvent.click(b);
    expect(onToggle).toHaveBeenCalled();
  });
});

describe('ActionRow', () => {
  it('is one button with its label', async () => {
    const onclick = vi.fn();
    const { container } = render(ActionRow, { icon: 'mac', label: 'New Machine...', onclick });
    const b = container.querySelector('button.gs-action-row')!;
    expect(b.textContent).toContain('New Machine...');
    await fireEvent.click(b);
    expect(onclick).toHaveBeenCalled();
  });
});
