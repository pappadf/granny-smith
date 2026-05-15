import { render, fireEvent } from '@testing-library/svelte';
import { describe, it, expect, beforeEach } from 'vitest';
import Toast from '@/components/common/Toast.svelte';
import { _resetForTests, showNotification, toasts } from '@/state/toasts.svelte';

beforeEach(() => _resetForTests());

describe('Toast', () => {
  it('renders severity glyph and message', () => {
    showNotification('hello world', 'info');
    const { container } = render(Toast, { toast: toasts.active[0] });
    expect(container.querySelector('.sev-icon')?.textContent).toBe('i');
    expect(container.querySelector('.msg')?.textContent).toBe('hello world');
  });

  it.each([
    ['info', 'i'],
    ['warning', '!'],
    ['error', 'x'],
  ] as const)('uses %s severity glyph %s', (severity, glyph) => {
    showNotification('m', severity);
    const { container } = render(Toast, { toast: toasts.active[0] });
    expect(container.querySelector('.sev-icon')?.classList.contains(severity)).toBe(true);
    expect(container.querySelector('.sev-icon')?.textContent).toBe(glyph);
  });

  it('close button dismisses the toast', async () => {
    showNotification('dismiss me');
    const id = toasts.active[0].id;
    const { container } = render(Toast, { toast: toasts.active[0] });
    const closeBtn = container.querySelector('.close-btn') as HTMLButtonElement;
    await fireEvent.click(closeBtn);
    expect(toasts.active.find((t) => t.id === id)).toBeUndefined();
  });
});
