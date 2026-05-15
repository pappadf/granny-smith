import { render, fireEvent, waitFor } from '@testing-library/svelte';
import { describe, it, expect, beforeEach } from 'vitest';
import WelcomeHomeSlide from '@/components/display/WelcomeHomeSlide.svelte';
import { _resetForTests, toasts } from '@/state/toasts.svelte';
import { layout, setWelcomeSlide } from '@/state/layout.svelte';
import { machine, stopDriveActivityMock } from '@/state/machine.svelte';
import { setOpfsBackend, MockOpfs } from '@/bus/opfs';

beforeEach(() => {
  _resetForTests();
  setWelcomeSlide('home');
  setOpfsBackend(new MockOpfs());
  machine.status = 'no-machine';
  machine.model = null;
  machine.ram = null;
  stopDriveActivityMock();
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

  it('Start card has three rows', () => {
    const { container } = render(WelcomeHomeSlide);
    const startCard = container.querySelectorAll('.card')[0];
    expect(startCard.querySelectorAll('.card-row').length).toBe(3);
  });

  it('"New Machine..." switches to the Configuration slide (no toast)', async () => {
    const { container } = render(WelcomeHomeSlide);
    const newMachineBtn = container.querySelector('.card-row') as HTMLButtonElement;
    await fireEvent.click(newMachineBtn);
    expect(layout.welcomeSlide).toBe('configuration');
    expect(toasts.active).toEqual([]);
  });

  it('"Open Checkpoint..." fires a warning toast pointing at Phase 3', async () => {
    const { container } = render(WelcomeHomeSlide);
    const buttons = container.querySelectorAll('.card-row');
    await fireEvent.click(buttons[1] as HTMLButtonElement);
    expect(toasts.active.length).toBe(1);
    expect(toasts.active[0].severity).toBe('warning');
    expect(toasts.active[0].msg).toContain('Phase 3');
  });

  it('renders the Recent card after loading recents from OPFS', async () => {
    const { container } = render(WelcomeHomeSlide);
    await waitFor(() => {
      const headings = Array.from(container.querySelectorAll('.card-heading')).map(
        (h) => h.textContent,
      );
      expect(headings).toContain('Recent');
    });
    const recentCard = container.querySelectorAll('.card')[1];
    const rows = recentCard.querySelectorAll('.card-row.recent');
    expect(rows.length).toBeGreaterThan(0);
  });

  it('clicking a Recent entry boots the mock machine', async () => {
    const { container } = render(WelcomeHomeSlide);
    const recentRow = await waitFor(() => {
      const r = container.querySelector('.card-row.recent') as HTMLButtonElement | null;
      if (!r) throw new Error('not yet');
      return r;
    });
    await fireEvent.click(recentRow);
    expect(machine.status).toBe('running');
    expect(machine.model).toBeTruthy();

    stopDriveActivityMock();
  });
});
