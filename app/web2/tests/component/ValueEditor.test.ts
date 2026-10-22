// ValueEditor: a toggle follows its value when the value is written another
// way (S1018-75: Enter on a SYSTEM bool row wrote it, the switch kept the old).
import { render } from '@testing-library/svelte';
import { describe, it, expect } from 'vitest';
import { tick } from 'svelte';
import ValueEditor from '@/components/common/ValueEditor.svelte';

describe('ValueEditor', () => {
  it('a bool toggle shows a new value from outside', async () => {
    const type = { kind: 'bool' } as never;
    const { container, rerender } = render(ValueEditor, {
      type,
      value: 'false',
      onCommit: () => null,
    });
    const sw = () => container.querySelector('[role="switch"]')?.getAttribute('aria-checked');
    expect(sw()).toBe('false');
    await rerender({ type, value: 'true', onCommit: () => null });
    await tick();
    expect(sw()).toBe('true');
  });
});
