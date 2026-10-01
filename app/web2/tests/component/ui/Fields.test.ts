import { render, fireEvent } from '@testing-library/svelte';
import { describe, it, expect, vi } from 'vitest';
import { createRawSnippet } from 'svelte';
import TextInput from '@/components/ui/TextInput.svelte';
import InlineValueInput from '@/components/ui/InlineValueInput.svelte';
import Select from '@/components/ui/Select.svelte';
import Checkbox from '@/components/ui/Checkbox.svelte';
import RadioGroup from '@/components/ui/RadioGroup.svelte';
import Field from '@/components/ui/Field.svelte';

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
});
