import { render, fireEvent } from '@testing-library/svelte';
import { describe, it, expect, beforeEach } from 'vitest';
import WelcomeConfigSlide from '@/components/display/WelcomeConfigSlide.svelte';
import { machine, stopDriveActivityMock } from '@/state/machine.svelte';
import { layout } from '@/state/layout.svelte';
import { _resetForTests } from '@/state/toasts.svelte';

beforeEach(() => {
  _resetForTests();
  machine.status = 'no-machine';
  machine.model = null;
  machine.ram = null;
  layout.welcomeSlide = 'configuration';
  // Make sure no leftover interval keeps timers alive between tests.
  stopDriveActivityMock();
});

describe('WelcomeConfigSlide', () => {
  it('renders all six form fields with prototype defaults', () => {
    const { container } = render(WelcomeConfigSlide);
    expect((container.querySelector('#cfg-model') as HTMLSelectElement).value).toBe(
      'Macintosh Plus',
    );
    expect((container.querySelector('#cfg-vrom') as HTMLSelectElement).value).toBe('(auto)');
    expect((container.querySelector('#cfg-ram') as HTMLSelectElement).value).toBe('4 MB');
    expect((container.querySelector('#cfg-fd') as HTMLSelectElement).value).toBe('(none)');
    expect((container.querySelector('#cfg-hd') as HTMLSelectElement).value).toBe('hd1.img');
    expect((container.querySelector('#cfg-cd') as HTMLSelectElement).value).toBe('(none)');
  });

  it('Back link returns to the home slide', async () => {
    const { container } = render(WelcomeConfigSlide);
    const backLink = container.querySelector('.back-link') as HTMLAnchorElement;
    await fireEvent.click(backLink);
    expect(layout.welcomeSlide).toBe('home');
  });

  it('Start Machine flips machine.status to running with collected config', async () => {
    const { container } = render(WelcomeConfigSlide);
    const form = container.querySelector('form') as HTMLFormElement;
    await fireEvent.submit(form);
    expect(machine.status).toBe('running');
    expect(machine.model).toBe('Macintosh Plus');
    expect(machine.ram).toBe('4 MB');
    expect(machine.mmuEnabled).toBe(false); // Plus has no MMU
    // After submit, the slide resets to home for the next visit.
    expect(layout.welcomeSlide).toBe('home');

    stopDriveActivityMock();
  });

  it('mmuEnabled becomes true when an MMU-capable model is selected', async () => {
    const { container } = render(WelcomeConfigSlide);
    const modelSelect = container.querySelector('#cfg-model') as HTMLSelectElement;
    modelSelect.value = 'Macintosh SE/30';
    await fireEvent.change(modelSelect);
    const form = container.querySelector('form') as HTMLFormElement;
    await fireEvent.submit(form);
    expect(machine.mmuEnabled).toBe(true);
    expect(machine.model).toBe('Macintosh SE/30');

    stopDriveActivityMock();
  });
});
