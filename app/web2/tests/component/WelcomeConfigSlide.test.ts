import { render, fireEvent, waitFor } from '@testing-library/svelte';
import { describe, it, expect, beforeEach } from 'vitest';
import WelcomeConfigSlide from '@/components/display/WelcomeConfigSlide.svelte';
import { machine, stopDriveActivityMock } from '@/state/machine.svelte';
import { layout, setWelcomeSlide } from '@/state/layout.svelte';
import { _resetForTests } from '@/state/toasts.svelte';
import { setOpfsBackend, MockOpfs } from '@/bus/opfs';

beforeEach(() => {
  _resetForTests();
  setOpfsBackend(new MockOpfs());
  machine.status = 'no-machine';
  machine.model = null;
  machine.ram = null;
  setWelcomeSlide('configuration');
  stopDriveActivityMock();
});

describe('WelcomeConfigSlide', () => {
  it('renders all six form fields with prototype defaults', async () => {
    const { container } = render(WelcomeConfigSlide);
    // The ROM select is async — wait for the OPFS scan to populate it.
    await waitFor(() => {
      const sel = container.querySelector('#cfg-rom') as HTMLSelectElement | null;
      if (!sel) throw new Error('not ready');
    });
    expect((container.querySelector('#cfg-model') as HTMLSelectElement).value).toBe(
      'Macintosh Plus',
    );
    expect((container.querySelector('#cfg-vrom') as HTMLSelectElement).value).toBe('(auto)');
    expect((container.querySelector('#cfg-ram') as HTMLSelectElement).value).toBe('4 MB');
    expect((container.querySelector('#cfg-fd') as HTMLSelectElement).value).toBe('(none)');
    expect((container.querySelector('#cfg-cd') as HTMLSelectElement).value).toBe('(none)');
  });

  it('Back link returns to the home slide', async () => {
    const { container } = render(WelcomeConfigSlide);
    await waitFor(() => {
      if (!container.querySelector('.back-link')) throw new Error('not ready');
    });
    const backLink = container.querySelector('.back-link') as HTMLAnchorElement;
    await fireEvent.click(backLink);
    expect(layout.welcomeSlide).toBe('home');
  });

  it('Start Machine submit attempts boot (no-op without Module, but resets slide)', async () => {
    const { container } = render(WelcomeConfigSlide);
    await waitFor(() => {
      if (!container.querySelector('#cfg-rom')) throw new Error('not ready');
    });
    const form = container.querySelector('form') as HTMLFormElement;
    await fireEvent.submit(form);
    // The form's submit handler resets the slide so the next Welcome lands
    // on Home. Without a real Module the gsEval calls are no-ops, so
    // machine.status doesn't change.
    expect(layout.welcomeSlide).toBe('home');
  });
});
