import { render, waitFor } from '@testing-library/svelte';
import { describe, it, expect, vi } from 'vitest';
import SystemView from '@/components/panel-views/machine/SystemView.svelte';

// Mock the emulator bus so the faithful walk sees a "ready" Module and a
// synthetic root: the machine container plus one meta object. There is no
// allowlist — SystemView renders whatever children the root's meta.members
// lists, labelled and grouped from the model.
vi.mock('@/bus/emulator', () => {
  return {
    isModuleReady: () => true,
    gsEval: async (path: string) => {
      // The root's members: its children carry their own label and category.
      if (path === 'meta.members')
        return [
          { name: 'machine', kind: 'child', category: 'basic', label: 'Macintosh IIcx', doc: '' },
          { name: 'storage', kind: 'child', category: 'basic', label: 'Storage', doc: '' },
          { name: 'secret', kind: 'child', category: 'internal', label: 'Secret', doc: '' },
          { name: 'echo', kind: 'method', category: 'basic', label: 'echo', doc: '' },
        ];
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
      expect(labels).not.toContain('Secret'); // internal nodes are never shown
    });
    // Meta objects sit under the non-interactive "Emulator" divider.
    const dividers = Array.from(container.querySelectorAll('.group-divider')).map(
      (e) => e.textContent,
    );
    expect(dividers).toContain('Emulator');
  });
});
