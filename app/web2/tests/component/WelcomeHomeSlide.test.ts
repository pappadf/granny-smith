import { render, fireEvent } from '@testing-library/svelte';
import { describe, it, expect, beforeEach } from 'vitest';
import WelcomeHomeSlide from '@/components/display/WelcomeHomeSlide.svelte';
import { _resetForTests, toasts } from '@/state/toasts.svelte';
import { layout, setWelcomeSlide } from '@/state/layout.svelte';
import { machine } from '@/state/machine.svelte';
import { setOpfsBackend } from '@/bus/opfs';
import { MockOpfs } from '../helpers/mockOpfs';

beforeEach(() => {
  _resetForTests();
  setWelcomeSlide('home');
  setOpfsBackend(new MockOpfs());
  machine.status = 'no-machine';
  machine.model = null;
  machine.ram = null;
});

describe('WelcomeHomeSlide', () => {
  it('renders title, subtitle, and Start card', () => {
    const { container } = render(WelcomeHomeSlide);
    expect(container.querySelector('.welcome-title')?.textContent).toBe('Granny Smith');
    const headings = Array.from(container.querySelectorAll('.card-heading')).map(
      (h) => h.textContent,
    );
    expect(headings[0]).toBe('Start');
  });

  it('"New Machine..." switches to the Configuration slide (no toast)', async () => {
    const { container } = render(WelcomeHomeSlide);
    const newMachineBtn = container.querySelector('.card-row') as HTMLButtonElement;
    await fireEvent.click(newMachineBtn);
    expect(layout.welcomeSlide).toBe('configuration');
    expect(toasts.active).toEqual([]);
  });

  it('offers New Machine and Upload ROM, and nothing that is not built', () => {
    const { container } = render(WelcomeHomeSlide);
    const rows = Array.from(container.querySelectorAll('.card-row')).map((b) =>
      b.textContent?.trim(),
    );
    expect(rows).toEqual(['New Machine...', 'Upload ROM...']);
  });

  // Nothing in production ever wrote a recent list (N-13): no Recent card.
  it('shows only the Start card', () => {
    const { container } = render(WelcomeHomeSlide);
    const headings = Array.from(container.querySelectorAll('.card-heading')).map(
      (h) => h.textContent,
    );
    expect(headings).toEqual(['Start']);
  });
});
