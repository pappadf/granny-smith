import { render, waitFor } from '@testing-library/svelte';
import { describe, it, expect, vi } from 'vitest';
import SystemView from '@/components/panel-views/machine/SystemView.svelte';

// Mock the emulator bus so the faithful walk sees a "ready" Module and a
// synthetic root: the machine container plus one meta object. There is no
// allowlist — SystemView renders whatever `objects` (the root's children)
// returns, labelled and grouped from the model (proposal §8.2).
vi.mock('@/bus/emulator', () => {
  return {
    isModuleReady: () => true,
    gsEval: async (path: string) => {
      if (path === 'objects') return ['machine', 'storage'];
      if (path === 'machine.meta.category') return 'basic';
      if (path === 'machine.meta.label') return 'Macintosh IIcx';
      if (path === 'storage.meta.category') return 'basic';
      if (path === 'storage.meta.label') return 'Storage';
      return null;
    },
  };
});

describe('SystemView', () => {
  it('renders the root children faithfully, labelled + grouped from the model', async () => {
    const { container } = render(SystemView);
    await waitFor(() => {
      const labels = Array.from(container.querySelectorAll('.label')).map((e) => e.textContent);
      // The machine container leads (model-owned label), the meta object follows.
      expect(labels).toContain('Macintosh IIcx');
      expect(labels).toContain('Storage');
    });
    // Meta objects sit under the non-interactive "Emulator" divider (§8.2).
    const dividers = Array.from(container.querySelectorAll('.group-divider')).map(
      (e) => e.textContent,
    );
    expect(dividers).toContain('Emulator');
  });
});
