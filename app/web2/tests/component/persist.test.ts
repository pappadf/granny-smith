import { describe, it, expect, beforeEach } from 'vitest';
import { loadPersistedState } from '@/state/persist.svelte';
import { theme } from '@/state/theme.svelte';
import { layout } from '@/state/layout.svelte';
import { debug } from '@/state/debug.svelte';
import { logs } from '@/state/logs.svelte';
import { filesystem } from '@/state/filesystem.svelte';
import { images } from '@/state/images.svelte';
import { checkpoints } from '@/state/checkpoints.svelte';

const V1 = (data: unknown) => JSON.stringify({ v: 1, data });

beforeEach(() => {
  localStorage.clear();
});

describe('loadPersistedState — Phase 7 keys', () => {
  it('restores Debug section expansion from gs-debug-sections', () => {
    localStorage.setItem('gs-debug-sections', V1({ registers: false, memory: true, mmu: true }));
    loadPersistedState();
    expect(debug.sections.registers).toBe(false);
    expect(debug.sections.memory).toBe(true);
    expect(debug.sections.mmu).toBe(true);
  });

  it('restores Memory addr + mode', () => {
    localStorage.setItem('gs-debug-memory', V1({ address: 0x00abcd00, mode: 'physical' }));
    loadPersistedState();
    expect(debug.memoryAddress).toBe(0x00abcd00);
    expect(debug.memoryMode).toBe('physical');
  });

  it('restores MMU subtab + supervisor', () => {
    localStorage.setItem('gs-debug-mmu', V1({ subtab: 'descriptors', supervisor: true }));
    loadPersistedState();
    expect(debug.mmuSubtab).toBe('descriptors');
    expect(debug.mmuSupervisor).toBe(true);
  });

  it('restores logs.autoscroll', () => {
    localStorage.setItem('gs-logs-autoscroll', V1(false));
    loadPersistedState();
    expect(logs.autoscroll).toBe(false);
  });

  it('restores filesystem.expanded', () => {
    localStorage.setItem('gs-fs-expanded', V1({ '/opfs': true, '/opfs /opfs/images': true }));
    loadPersistedState();
    expect(filesystem.expanded['/opfs']).toBe(true);
    expect(filesystem.expanded['/opfs /opfs/images']).toBe(true);
  });

  it('restores images.collapsed', () => {
    localStorage.setItem('gs-images-collapsed', V1({ rom: true, vrom: false }));
    loadPersistedState();
    expect(images.collapsed.rom).toBe(true);
    expect(images.collapsed.vrom).toBe(false);
  });

  it('restores checkpoints sort', () => {
    localStorage.setItem('gs-checkpoints-sort', V1({ column: 'machine', dir: 'asc' }));
    loadPersistedState();
    expect(checkpoints.sortColumn).toBe('machine');
    expect(checkpoints.sortDir).toBe('asc');
  });

  it('tolerates malformed JSON without throwing', () => {
    localStorage.setItem('gs-debug-sections', '{this is not json');
    expect(() => loadPersistedState()).not.toThrow();
  });

  it('rejects wrong-version envelopes (gracefully falls back)', () => {
    localStorage.setItem('gs-debug-mmu', JSON.stringify({ v: 99, data: { subtab: 'state' } }));
    debug.mmuSubtab = 'translate';
    loadPersistedState();
    // Wrong-version envelope is ignored — the existing state stays.
    expect(debug.mmuSubtab).toBe('translate');
  });

  it('still restores the Phase 3 keys (theme + panelPos + panelSize)', () => {
    localStorage.setItem('gs-theme', 'light');
    localStorage.setItem('gs-panel-pos', 'left');
    localStorage.setItem('gs-panel-size', JSON.stringify({ left: 320 }));
    loadPersistedState();
    expect(theme.mode).toBe('light');
    expect(layout.panelPos).toBe('left');
    expect(layout.panelSize.left).toBe(320);
  });
});
