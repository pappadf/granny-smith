import { describe, it, expect, vi } from 'vitest';

// Mock the bus so loadSystemChildren walks a synthetic SCSI device collection:
// machine.scsi.device has one indexed member (`entries`) with live slots 0 and
// 3. The walk must expand it into machine.scsi.device[0] / [3] entries rather
// than showing the bare `entries` member (proposal §5.3 — the bug the SYSTEM
// tab had where indexed collections never enumerated).
vi.mock('@/bus/emulator', () => ({
  isModuleReady: () => true,
  gsEval: async (path: string, args?: unknown[]) => {
    const arg = args?.[0];
    if (path === 'machine.scsi.device.meta.attributes') return [];
    if (path === 'machine.scsi.device.meta.children') return ['bus', 'entries'];
    if (path === 'machine.scsi.device.meta.member_category') return 'basic';
    if (path === 'machine.scsi.device.meta.member_label') return arg;
    // entries is the indexed member → live slots; bus is a plain named child.
    if (path === 'machine.scsi.device.meta.indices') {
      if (arg === 'entries') return [0, 3];
      return { error: "indices: 'bus' is not an indexed child" };
    }
    return null;
  },
}));

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
});
