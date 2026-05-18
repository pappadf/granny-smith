import { render, waitFor } from '@testing-library/svelte';
import { describe, it, expect, vi, beforeEach } from 'vitest';
import MachineView from '@/components/panel-views/machine/MachineView.svelte';

// Mock the emulator bus so loadMachineRoots / loadMachineChildren see a
// "ready" Module and return synthetic data.
vi.mock('@/bus/emulator', () => {
  return {
    isModuleReady: () => true,
    gsEval: async (path: string) => {
      if (path === 'cpu.meta.children') return ['flags'];
      if (path === 'cpu.meta.attributes') return ['pc'];
      if (path === 'cpu.pc') return 0x40028e;
      if (path === 'memory.meta.children') return [];
      if (path === 'memory.meta.attributes') return ['ram_kb'];
      if (path === 'memory.ram_kb') return 4096;
      // All other subtrees: return null so they get filtered out.
      return null;
    },
  };
});

beforeEach(() => {
  // Reset machine status so the $effect doesn't re-fire mid-test.
});

describe('MachineView', () => {
  it('lists the subtrees whose meta call succeeds', async () => {
    const { container } = render(MachineView);
    await waitFor(() => {
      const labels = Array.from(container.querySelectorAll('.label')).map((e) => e.textContent);
      expect(labels).toContain('CPU');
      expect(labels).toContain('Memory');
      // Roots that returned null are dropped.
      expect(labels).not.toContain('SCSI');
      expect(labels).not.toContain('Sound');
    });
  });
});
