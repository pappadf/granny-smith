import { render, fireEvent } from '@testing-library/svelte';
import { describe, it, expect, beforeEach } from 'vitest';
import WelcomeHomeSlide from '@/components/display/WelcomeHomeSlide.svelte';
import { _resetForTests, toasts } from '@/state/toasts.svelte';

beforeEach(() => _resetForTests());

describe('WelcomeHomeSlide', () => {
  it('renders title, subtitle, Start card, and Recent card', () => {
    const { container } = render(WelcomeHomeSlide);
    expect(container.querySelector('.welcome-title')?.textContent).toBe('Granny Smith');
    const headings = Array.from(container.querySelectorAll('.card-heading')).map(
      (h) => h.textContent,
    );
    expect(headings).toEqual(['Start', 'Recent']);
  });

  it('Start card has three rows', () => {
    const { container } = render(WelcomeHomeSlide);
    const startCard = container.querySelectorAll('.card')[0];
    expect(startCard.querySelectorAll('.card-row').length).toBe(3);
  });

  it('clicking a Start button fires a warning toast', async () => {
    const { container } = render(WelcomeHomeSlide);
    const newMachineBtn = container.querySelector('.card-row') as HTMLButtonElement;
    await fireEvent.click(newMachineBtn);
    expect(toasts.active.length).toBe(1);
    expect(toasts.active[0].severity).toBe('warning');
  });
});
