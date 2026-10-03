import { render, fireEvent } from '@testing-library/svelte';
import { describe, it, expect, vi } from 'vitest';
import { createRawSnippet } from 'svelte';
import TextInput from '@/components/ui/TextInput.svelte';
import InlineValueInput from '@/components/ui/InlineValueInput.svelte';
import Select from '@/components/ui/Select.svelte';
import Checkbox from '@/components/ui/Checkbox.svelte';
import RadioGroup from '@/components/ui/RadioGroup.svelte';
import Field from '@/components/ui/Field.svelte';
import FormGrid from '@/components/ui/FormGrid.svelte';
import NumberInput from '@/components/ui/NumberInput.svelte';
import Link from '@/components/ui/Link.svelte';

// A raw snippet renders a single element: one option.
const options = createRawSnippet(() => ({ render: () => '<option value="a">A</option>' }));

describe('TextInput', () => {
  it('forwards attributes and maps invalid to aria-invalid', () => {
    const { container } = render(TextInput, {
      id: 'rename-input',
      value: 'x/y',
      invalid: true,
      'aria-label': 'Name',
    });
    const i = container.querySelector('input#rename-input')!;
    expect(i.getAttribute('aria-invalid')).toBe('true');
    expect(i.getAttribute('aria-label')).toBe('Name');
    expect((i as HTMLInputElement).value).toBe('x/y');
  });

  it('hex fields are monospace and upper-cased', () => {
    const { container } = render(TextInput, { hex: true, value: 'beef' });
    const i = container.querySelector('input')!;
    expect(i.hasAttribute('data-hex')).toBe(true);
    expect(i.hasAttribute('data-mono')).toBe(true);
  });
});

describe('InlineValueInput', () => {
  it('changed and invalid are state attributes', () => {
    const { container } = render(InlineValueInput, { value: '1', changed: true, invalid: true });
    const i = container.querySelector('input')!;
    expect(i.getAttribute('data-state')).toBe('changed');
    expect(i.getAttribute('aria-invalid')).toBe('true');
  });
});

describe('Select', () => {
  it('renders a native select with its id and options, and reports changes', async () => {
    const onchange = vi.fn();
    const { container } = render(Select, {
      id: 'cfg-ram',
      value: 'a',
      onchange,
      children: options,
    });
    const s = container.querySelector('select#cfg-ram') as HTMLSelectElement;
    expect(s.options.length).toBe(1);
    await fireEvent.change(s);
    expect(onchange).toHaveBeenCalled();
  });
});

describe('Checkbox', () => {
  it('labels its box and reports the new state', async () => {
    const onchange = vi.fn();
    const { container } = render(Checkbox, { label: 'Advanced', class: 'adv-toggle', onchange });
    const box = container.querySelector('.adv-toggle input[type=checkbox]') as HTMLInputElement;
    expect(container.querySelector('.adv-toggle')!.textContent).toContain('Advanced');
    await fireEvent.click(box);
    expect(onchange).toHaveBeenCalledWith(true);
  });
});

describe('RadioGroup', () => {
  it('is a named group of radios with the value checked', () => {
    const { container } = render(RadioGroup, {
      name: 'fd',
      label: 'Capacity',
      value: '1440K',
      options: [
        { value: '800K', label: '800 KB' },
        { value: '1440K', label: '1.4 MB' },
      ],
    });
    const checked = container.querySelector('input[type=radio]:checked') as HTMLInputElement;
    expect(checked.value).toBe('1440K');
    expect(container.querySelector('legend')!.textContent).toBe('Capacity');
  });
});

describe('Field', () => {
  it('ties the label to its control and announces an error', () => {
    const { container } = render(Field, { label: 'RAM', for: 'cfg-ram', error: 'Too much' });
    expect(container.querySelector('label[for=cfg-ram]')!.textContent).toBe('RAM');
    expect(container.querySelector('[role=alert]')!.textContent).toBe('Too much');
  });

  it('a help-only row carries the legacy form-help class', () => {
    const { container } = render(Field, { help: 'Scanning ROMs…' });
    expect(container.querySelector('.form-help')!.textContent).toBe('Scanning ROMs…');
  });

  it('a stacked field without a label has no label row', () => {
    const { container } = render(Field, { stacked: true, error: 'Nope' });
    expect(container.querySelector('.gs-field__label')).toBeNull();
    expect(container.querySelector('.gs-field__error')!.textContent).toBe('Nope');
  });

  it('labelContent replaces the plain label with markup', () => {
    const labelContent = createRawSnippet(() => ({
      render: () => '<span class="arg-name">addr <i>int</i></span>',
    }));
    const { container } = render(Field, { stacked: true, labelContent });
    expect(container.querySelector('.gs-field__label .arg-name')!.textContent).toBe('addr int');
  });
});

describe('FormGrid', () => {
  it('is a form that keeps its legacy class and forwards submit', async () => {
    const onsubmit = vi.fn((ev: Event) => ev.preventDefault());
    const children = createRawSnippet(() => ({ render: () => '<input name="a" />' }));
    const { container } = render(FormGrid, { class: 'config-form', onsubmit, children });
    const form = container.querySelector('form.gs-form.config-form') as HTMLFormElement;
    expect(form.querySelector('input[name=a]')).not.toBeNull();
    await fireEvent.submit(form);
    expect(onsubmit).toHaveBeenCalledOnce();
  });
});

describe('NumberInput', () => {
  it('is a number TextInput that marks itself invalid', () => {
    const { container } = render(NumberInput, { value: 4, invalid: true, 'aria-label': 'RAM' });
    const input = container.querySelector('input[aria-label=RAM]') as HTMLInputElement;
    expect(input.type).toBe('number');
    expect(input.value).toBe('4');
    expect(input.getAttribute('aria-invalid')).toBe('true');
  });
});

describe('Link', () => {
  const children = createRawSnippet(() => ({ render: () => '<span>Releases</span>' }));

  it('opens an external link in a new tab safely', () => {
    const { container } = render(Link, { href: 'https://example.org', external: true, children });
    const a = container.querySelector('a.gs-link') as HTMLAnchorElement;
    expect(a.getAttribute('href')).toBe('https://example.org');
    expect(a.target).toBe('_blank');
    expect(a.rel).toBe('noopener noreferrer');
  });

  it('a link that acts in the page has no target', async () => {
    const onclick = vi.fn((ev: Event) => ev.preventDefault());
    const { container } = render(Link, { onclick, class: 'back-link', children });
    const a = container.querySelector('a.gs-link.back-link') as HTMLAnchorElement;
    expect(a.target).toBe('');
    await fireEvent.click(a);
    expect(onclick).toHaveBeenCalledOnce();
  });
});
