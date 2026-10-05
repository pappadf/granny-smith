// lib/machineConfig.ts: editing a configuration document against a tree.
import { describe, it, expect } from 'vitest';
import * as mc from '@/lib/machineConfig';
import type { MachineProfile } from '@/bus/profile';
import { tree } from '../helpers/configTree';
import { decodeConfigParam, encodeConfigParam } from '@/lib/mediaUrl';
import { urlConfig } from '@/bus/urlMedia';

const profile = (id: string) => tree(id) as unknown as MachineProfile;

// The Quadra 900's two SCSI connectors are one bus: one ID space.
function q900(): MachineProfile {
  const p = profile('iix');
  const units = [0, 1, 2, 3, 4, 5, 6].map((u) => ({ unit: u, label: `ID ${u}` }));
  p.storage = [
    { ...p.storage[0], id: 'scsi', shares_units_with: ['scsi2'] },
    { ...p.storage[0], id: 'scsi2', label: 'External SCSI', units, startup: false, bays: [] },
  ];
  return p;
}

describe('storage', () => {
  it('a unit taken on one connector is taken on the other', () => {
    const p = q900();
    const doc = mc.defaultDocument(p); // scsi 0 hd, scsi 3 cd
    expect(mc.freeUnits(p, doc, 'scsi2').map((u) => u.unit)).toEqual([1, 2, 4, 5, 6]);
  });

  it('a new device goes where the default configuration puts its type, else a bay, else the lowest', () => {
    const p = profile('plus');
    let doc = mc.defaultDocument(p);
    expect(mc.defaultUnit(p, doc, 'scsi', 'hd')).toBe(1);
    doc = mc.removeDevice(p, doc, 0);
    expect(mc.defaultUnit(p, doc, 'scsi', 'hd')).toBe(0);
  });

  it('removing or moving the startup device carries the startup choice', () => {
    const p = profile('plus');
    let doc = mc.addDevice(p, mc.defaultDocument(p), 'scsi', 'hd', 4);
    doc = mc.setDeviceUnit(doc, 0, 2);
    expect(doc.startup).toEqual({ bus: 'scsi', unit: 2 });
    doc = mc.removeDevice(p, doc, 0);
    expect(doc.startup).toEqual({ bus: 'scsi', unit: 4 });
    doc = mc.removeDevice(
      p,
      doc,
      doc.storage.findIndex((d) => d.unit === 4),
    );
    expect(doc.startup).toBeNull();
  });

  it('the startup list holds only devices on a bus whose record can name them', () => {
    const p = profile('pmg3dt');
    const doc = mc.addDevice(p, mc.defaultDocument(p), 'ata0', 'hd');
    expect(mc.startupChoices(p, doc).map((s) => s.bus)).toEqual(['scsi', 'scsi']);
  });

  it('hard disks with no image are dropped; a CD-ROM drive stays', () => {
    const p = profile('plus');
    const { doc, dropped } = mc.dropImagelessDisks(p, mc.defaultDocument(p), {});
    expect(dropped).toEqual([{ bus: 'scsi', unit: 0, type: 'hd' }]);
    expect(doc.storage).toEqual([{ bus: 'scsi', unit: 3, type: 'cd' }]);
    expect(doc.startup).toBeNull();
  });
});

describe('cards and displays', () => {
  it('a slot excluded by an occupied one is not free', () => {
    const p = profile('iici');
    p.slots[1].excludes = ['nubus_c'];
    const doc = mc.addCard(p, mc.defaultDocument(p), 'mdc_8_24', 'nubus_c');
    expect(mc.freeSlotsFor(p, doc, 'mdc_8_24').map((s) => s.id)).toEqual(['nubus_e']);
  });

  it('removing the connected card moves the monitor to the first remaining device', () => {
    const p = profile('iici');
    let doc = mc.addCard(p, mc.defaultDocument(p), 'mdc_8_24');
    doc = mc.connectTo(p, doc, 'nubus_c');
    expect(mc.connectedDevice(doc)).toBe('nubus_c');
    expect(doc.displays.builtin.monitor).toBe('none');
    doc = mc.removeCard(p, doc, 'nubus_c');
    expect(mc.connectedDevice(doc)).toBe('builtin');
  });

  it('the first display card added to a machine with no screen gets the monitor', () => {
    const p = profile('pm9500');
    const doc = mc.addCard(p, mc.defaultDocument(p), 'mach64_gx');
    expect(mc.connectedDevice(doc)).toBe('pci_1');
    expect(mc.configWarnings(p, doc)).toEqual([]);
    expect(mc.configWarnings(p, mc.defaultDocument(p))).toHaveLength(1);
  });
});

describe('the URL’s config=', () => {
  it('round-trips a document as base64url', () => {
    const doc = { model: 'iix', storage: [{ bus: 'scsi', unit: 0, type: 'hd' }], note: 'é+/' };
    const enc = encodeConfigParam(doc);
    expect(enc).not.toMatch(/[+/=]/);
    expect(decodeConfigParam(enc)).toEqual(doc);
    expect(decodeConfigParam('not json')).toBeNull();
  });

  it('fdN on a position the default leaves empty puts a drive there', () => {
    const p = profile('iix');
    expect(urlConfig(p, { config: null, floppies: [{ slot: 'fd0', url: 'a' }] })).toBeNull();
    const doc = urlConfig(p, { config: null, floppies: [{ slot: 'fd1', url: 'a' }] }) as {
      floppies: Record<string, string>;
    };
    expect(doc.floppies).toEqual({ fd0: 'hd', fd1: 'hd' });
    const given = { model: 'iix' };
    expect(urlConfig(p, { config: given, floppies: [{ slot: 'fd1', url: 'a' }] })).toBe(given);
  });
});
