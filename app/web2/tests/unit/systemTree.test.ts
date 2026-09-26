import { describe, it, expect, vi } from 'vitest';

// Mock the bus so loadSystemChildren walks a synthetic SCSI device collection:
// machine.scsi.device's meta.members lists one indexed member (`entries`)
// with live slots 0 and 3. The walk must expand it into machine.scsi.device[0] / [3] entries rather
// than showing the bare `entries` member (proposal §5.3 — the bug the SYSTEM
// tab had where indexed collections never enumerated).
vi.mock('@/bus/emulator', () => ({
  isModuleReady: () => true,
  gsEval: async (path: string, args?: unknown[]) => {
    calls.push(path);
    if (path !== 'machine.scsi.device.meta.members') return null;
    const values = args?.[0] === true;
    return [
      {
        name: 'count',
        kind: 'attr',
        category: 'basic',
        label: 'Count',
        doc: '',
        ...(values ? { value: 2 } : {}),
      },
      { name: 'raw', kind: 'attr', category: 'advanced', label: 'raw', doc: '', value: 7 },
      { name: 'bus', kind: 'child', category: 'basic', label: 'bus', doc: '', indexed: false },
      {
        name: 'entries',
        kind: 'child',
        category: 'basic',
        label: 'entries',
        doc: '',
        indexed: true,
        indices: [0, 3],
      },
      { name: 'lcd', kind: 'child', category: 'advanced', label: 'Front Panel LCD', doc: '' },
    ];
  },
}));
const calls: string[] = [];

const { loadSystemChildren } = await import('@/bus/systemTree');

describe('systemTree indexed-collection enumeration', () => {
  it('expands an indexed child member into its live entries', async () => {
    const nodes = await loadSystemChildren(['machine.scsi.device'], false);
    const ids = nodes.map((n) => n.id);
    // The indexed member `entries` becomes device[0] / device[3] …
    expect(ids).toContain('machine.scsi.device[0]');
    expect(ids).toContain('machine.scsi.device[3]');
    // … and the bare `entries` member is NOT shown.
    expect(ids).not.toContain('machine.scsi.device.entries');
    // The plain named child `bus` is still shown normally.
    expect(ids).toContain('machine.scsi.device.bus');
  });

  it('asks once, and honours categories and labels from the model', async () => {
    calls.length = 0;
    const basic = await loadSystemChildren(['machine.scsi.device'], false);
    expect(calls).toEqual(['machine.scsi.device.meta.members']);
    const count = basic.find((n) => n.id === 'machine.scsi.device.count');
    expect(count).toMatchObject({ label: 'Count', desc: '2', leaf: true });
    expect(basic.map((n) => n.id)).not.toContain('machine.scsi.device.raw');
    expect(basic.map((n) => n.id)).not.toContain('machine.scsi.device.lcd');

    const all = await loadSystemChildren(['machine.scsi.device'], true);
    expect(all.find((n) => n.id === 'machine.scsi.device.lcd')?.label).toBe('Front Panel LCD');
  });
});
