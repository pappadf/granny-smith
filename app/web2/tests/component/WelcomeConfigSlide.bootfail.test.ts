import { render, waitFor } from '@testing-library/svelte';
import { describe, it, expect, vi } from 'vitest';
import WelcomeConfigSlide from '@/components/display/WelcomeConfigSlide.svelte';

// The emulator could not start: the dialog must say so rather than stay on
// "Scanning ROMs…" forever.
vi.mock('@/bus/emulator', async (importOriginal) => {
  const actual = await importOriginal<typeof import('@/bus/emulator')>();
  return {
    ...actual,
    whenModuleReady: () => Promise.reject(new Error('js_bridge version mismatch: C=6, JS=7')),
  };
});

describe('WelcomeConfigSlide when the emulator did not start', () => {
  it('shows the reason and stops scanning', async () => {
    const { container } = render(WelcomeConfigSlide);
    await waitFor(() => {
      expect(container.textContent ?? '').toContain('The emulator did not start');
    });
    expect(container.textContent ?? '').toContain('version mismatch');
    expect(container.textContent ?? '').not.toContain('Scanning ROMs');
  });
});
