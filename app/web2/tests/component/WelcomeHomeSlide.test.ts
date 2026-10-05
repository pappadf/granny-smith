import { render, fireEvent, screen, waitFor } from '@testing-library/svelte';
import { describe, it, expect, beforeEach } from 'vitest';
import WelcomeHomeSlide from '@/components/display/WelcomeHomeSlide.svelte';
import { _resetForTests, toasts } from '@/state/toasts.svelte';
import { layout, setWelcomeSlide } from '@/state/layout.svelte';
import { machine } from '@/state/machine.svelte';
import { setOpfsBackend } from '@/bus/opfs';
import { MockOpfs } from '../helpers/mockOpfs';
import { recent } from '@/state/recent.svelte';

beforeEach(() => {
  _resetForTests();
  setWelcomeSlide('home');
  setOpfsBackend(new MockOpfs());
  machine.status = 'no-machine';
  machine.model = null;
  machine.ram = null;
  recent.entries = [];
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

  it('offers New Machine, Open Checkpoint and Load ROM', () => {
    const { container } = render(WelcomeHomeSlide);
    const rows = Array.from(container.querySelectorAll('.card-row')).map((b) =>
      b.textContent?.trim(),
    );
    expect(rows).toEqual(['New Machine...', 'Open Checkpoint...', 'Load ROM...']);
  });

  // Nothing started yet: no Recent card.
  it('shows only the Start card with no recent machines', () => {
    const { container } = render(WelcomeHomeSlide);
    const headings = Array.from(container.querySelectorAll('.card-heading')).map(
      (h) => h.textContent,
    );
    expect(headings).toEqual(['Start']);
  });

  it('lists recent machines, disables one whose image is gone, and forgets one', async () => {
    recent.entries = [
      {
        config: { model: 'plus', rom: '/opfs/images/rom/plus-v3-4d1f8172.rom' },
        label: 'Macintosh Plus · 4 MB',
        lastUsed: Date.now() - 2 * 3600_000,
      },
      {
        config: {
          model: 'iicx',
          rom: '/opfs/images/rom/iix-iicx-se30-97221136.rom',
          media: [{ bus: 'scsi', unit: 0, type: 'hd', path: '/opfs/images/hd/gone.img' }],
        },
        label: 'Macintosh IIcx · 8 MB · gone.img',
        lastUsed: Date.now(),
      },
    ];
    const { container } = render(WelcomeHomeSlide);
    const headings = Array.from(container.querySelectorAll('.card-heading')).map(
      (h) => h.textContent,
    );
    expect(headings).toEqual(['Start', 'Recent']);
    const launches = () =>
      Array.from(container.querySelectorAll<HTMLButtonElement>('.recent-launch'));
    expect(launches()[0].textContent).toContain('2 hours ago');
    await waitFor(() => expect(launches()[1].disabled).toBe(true));
    expect(launches()[1].textContent).toContain('missing: gone.img');
    expect(launches()[0].disabled).toBe(false);

    await fireEvent.click(screen.getByRole('button', { name: /Forget Macintosh Plus/ }));
    expect(recent.entries.map((e) => e.config.model)).toEqual(['iicx']);
    await waitFor(() => expect(launches()).toHaveLength(1));
  });
});
