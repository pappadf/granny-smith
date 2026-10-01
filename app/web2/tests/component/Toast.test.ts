import { render, fireEvent } from '@testing-library/svelte';
import { describe, it, expect, beforeEach } from 'vitest';
import Toast from '@/components/common/Toast.svelte';
import { _resetForTests, showNotification, toasts } from '@/state/toasts.svelte';

beforeEach(() => _resetForTests());

describe('Toast', () => {
  it('renders severity icon and message', () => {
    showNotification('hello world', 'info');
    const { container } = render(Toast, { toast: toasts.active[0] });
    expect(container.querySelector('.sev-icon use')?.getAttribute('href')).toContain('#i-info');
    expect(container.querySelector('.msg')?.textContent).toBe('hello world');
  });

  it.each([
    ['info', 'info', 'info'],
    ['warning', 'warning', 'warning'],
    ['error', 'danger', 'error'],
  ] as const)('a %s toast has intent %s and the %s icon', (severity, intent, icon) => {
    showNotification('m', severity);
    const { container } = render(Toast, { toast: toasts.active[0] });
    expect(container.querySelector('.toast')?.getAttribute('data-intent')).toBe(intent);
    expect(container.querySelector('.sev-icon')?.getAttribute('data-icon')).toBe(icon);
  });

  it('the close button is always in the DOM', () => {
    showNotification('m');
    const { container } = render(Toast, { toast: toasts.active[0] });
    expect(container.querySelector('button.close-btn')).not.toBeNull();
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
