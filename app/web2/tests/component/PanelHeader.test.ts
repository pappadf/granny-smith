import { render, fireEvent } from '@testing-library/svelte';
import { describe, it, expect, beforeEach } from 'vitest';
import PanelHeader from '@/components/panel/PanelHeader.svelte';
import { layout, PANEL_TABS, setActiveTab } from '@/state/layout.svelte';

beforeEach(() => setActiveTab('terminal'));

describe('PanelHeader', () => {
  it('renders the 7 panel tabs in spec order', () => {
    const { container } = render(PanelHeader);
    const tabs = Array.from(container.querySelectorAll('.ptab')).map((t) =>
      t.getAttribute('data-tab'),
    );
    expect(tabs).toEqual([...PANEL_TABS]);
  });

  it('tab labels match the spec', () => {
    const { container } = render(PanelHeader);
    const labels = Array.from(container.querySelectorAll('.ptab')).map((t) =>
      t.textContent?.trim(),
    );
    expect(labels).toEqual([
      'TERMINAL',
      'MACHINE',
      'FILESYSTEM',
      'IMAGES',
      'CHECKPOINTS',
      'DEBUG',
      'LOGS',
    ]);
  });

  it('clicking a tab updates layout.activeTab', async () => {
    const { container } = render(PanelHeader);
    const machineTab = container.querySelector('[data-tab="machine"]') as HTMLButtonElement;
    await fireEvent.click(machineTab);
    expect(layout.activeTab).toBe('machine');
  });

  it('active tab has the .active class', async () => {
    const { container } = render(PanelHeader);
    await fireEvent.click(container.querySelector('[data-tab="logs"]') as HTMLButtonElement);
    const logsTab = container.querySelector('[data-tab="logs"]') as HTMLElement;
    expect(logsTab.classList.contains('active')).toBe(true);
  });
});
