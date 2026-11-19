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
    const p = profile('iix');
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

  // S1018-56: the Plus's ROM boots the highest SCSI ID.
  it('a model with no startup record has no startup device or choices', () => {
    const p = profile('plus');
    const doc = mc.addDevice(p, mc.defaultDocument(p), 'scsi', 'hd', 4);
    expect(doc.startup).toBeNull();
    expect(mc.startupChoices(p, doc)).toEqual([]);
  });

  it('the startup list holds only devices on a bus whose record can name them', () => {
    const p = q900();
    const doc = mc.addDevice(p, mc.defaultDocument(p), 'scsi2', 'hd');
    expect(mc.startupChoices(p, doc).map((s) => s.bus)).toEqual(['scsi', 'scsi']);
  });

  it('a disk whose volumes are known is one startup choice per volume, by name', () => {
    const p = profile('pmg3dt');
    const doc = mc.addDevice(p, mc.defaultDocument(p), 'ata0', 'hd');
    const volumes = {
      'ata0:0': [
        { partition: 3, name: 'Macintosh HD', filesystem: 'HFS+', powerpc_booter: true },
        { partition: 4, name: 'Data', filesystem: 'HFS', powerpc_booter: false },
      ],
    };
    const choices = mc.startupChoices(p, doc, volumes).filter((s) => s.bus === 'ata0');
    expect(choices.map((s) => mc.startupKey(s))).toEqual(['ata0:0:3', 'ata0:0:4']);
    expect(choices[0].label).toMatch(/· Macintosh HD$/);
  });

  it('the startup partition follows the disk: its Mac OS X volume, else its first', () => {
    const p = profile('pmg3dt');
    let doc = mc.addDevice(p, mc.defaultDocument(p), 'ata0', 'hd');
    doc = mc.setStartup(doc, { bus: 'ata0', unit: 0 });
    const vols = (booter: number) => ({
      'ata0:0': [3, 4].map((n) => ({
        partition: n,
        name: `V${n}`,
        filesystem: 'HFS+',
        powerpc_booter: n === booter,
      })),
    });
    // Not looked at yet: the document stands.
    expect(mc.settleStartupPartition(doc, {})).toBe(doc);
    doc = mc.settleStartupPartition(doc, vols(4));
    expect(doc.startup).toEqual({ bus: 'ata0', unit: 0, partition: 4 });
    // A chosen volume the disk has stays chosen.
    doc = mc.setStartup(doc, { bus: 'ata0', unit: 0, partition: 3 });
    expect(mc.settleStartupPartition(doc, vols(4))).toBe(doc);
    // Another image, with no volume there: the disk's default again.
    doc = mc.setStartup(doc, { bus: 'ata0', unit: 0, partition: 9 });
    expect(mc.settleStartupPartition(doc, vols(0)).startup?.partition).toBe(3);
    // A disk with no volumes names none.
    expect(mc.settleStartupPartition(doc, { 'ata0:0': [] }).startup).toEqual({
      bus: 'ata0',
      unit: 0,
    });
  });

  it('the startup list names devices by unit number, not by bay', () => {
    const p = profile('iici');
    expect(mc.startupChoices(p, mc.defaultDocument(p)).map((s) => s.label)).toEqual([
      'Hard disk · ID 0',
      'CD-ROM drive · ID 3',
    ]);
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

// A 9500 with the Symbios SCSI card on offer and the bus it brings in slot
// pci_2, as catalog.profile describes a PCI card's bus: a storage bus whose
// card_slot names the slot, and a card with scsi_bus.
function pm9500WithScsiCard(): MachineProfile {
  const p = profile('pm9500');
  const units = [0, 1, 2, 3, 4, 5, 6].map((u) => ({ unit: u, label: `ID ${u}`, short: `ID ${u}` }));
  p.storage.push({
    ...p.storage[0],
    id: 'pci_2',
    label: 'SCSI card in PCI slot 2',
    units,
    reserved: [7],
    bays: [],
    startup: false,
    card_slot: 'pci_2',
  });
  p.cards.push({
    id: 'sym53c825',
    label: 'Symbios 53C825A SCSI card',
    class: 'scsi',
    fits: ['pci_1', 'pci_2', 'pci_3'],
    rom: null,
    status: 'ok',
    monitors: [],
    modes: {},
    options: [],
    scsi_bus: true,
  });
  return p;
}

describe('a PCI card that brings its own SCSI bus', () => {
  it('its bus is offered only while the card is in that slot', () => {
    const p = pm9500WithScsiCard();
    let doc = mc.defaultDocument(p);
    expect(mc.visibleBuses(p, doc).map((b) => b.id)).toEqual(['scsi']);
    doc = mc.addCard(p, doc, 'sym53c825', 'pci_1');
    expect(mc.visibleBuses(p, doc).map((b) => b.id)).toEqual(['scsi']);
    doc = mc.addCard(p, doc, 'sym53c825', 'pci_2');
    expect(mc.visibleBuses(p, doc).map((b) => b.id)).toEqual(['scsi', 'pci_2']);
  });

  it('removing the card takes its bus and the devices on it', () => {
    const p = pm9500WithScsiCard();
    let doc = mc.addCard(p, mc.defaultDocument(p), 'sym53c825', 'pci_2');
    doc = mc.addDevice(p, doc, 'pci_2', 'hd', 0);
    doc = mc.addDevice(p, doc, 'pci_2', 'cd', 3);
    expect(doc.storage.filter((d) => d.bus === 'pci_2')).toHaveLength(2);
    doc = mc.removeCard(p, doc, 'pci_2');
    expect(doc.storage.filter((d) => d.bus === 'pci_2')).toEqual([]);
    expect(mc.visibleBuses(p, doc).map((b) => b.id)).toEqual(['scsi']);
  });

  it('a card without a bus leaves the storage alone when it goes', () => {
    const p = pm9500WithScsiCard();
    let doc = mc.addCard(p, mc.defaultDocument(p), 'mach64_gx', 'pci_3');
    const before = JSON.stringify(doc.storage);
    doc = mc.removeCard(p, doc, 'pci_3');
    expect(JSON.stringify(doc.storage)).toBe(before);
  });
});
