// Editing a configuration document against its machine-description tree.
//
// Pure functions over bus/profile.ts's types: the dialog renders the tree
// and calls these to change the document.  Nothing here knows a machine, a
// bus or a card -- every fact comes from the tree (which slots exist, what
// fits them, which units are free, what each thing is called), so a new
// machine or card needs no change here.

import type {
  Card,
  CardEntry,
  Choice,
  ConfigDocument,
  ConfigOption,
  MachineProfile,
  StorageBus,
  StorageDevice,
  StorageUnit,
} from '@/bus/profile';

// The document id of the built-in display device.
export const BUILTIN = 'builtin';
// The monitor an unplugged port senses.
export const NO_MONITOR = 'none';

// A deep copy, so an edit never mutates the profile's defaults.
export function cloneDocument(doc: ConfigDocument): ConfigDocument {
  return JSON.parse(JSON.stringify(doc)) as ConfigDocument;
}

// The model's default configuration, ready to edit.
export function defaultDocument(profile: MachineProfile): ConfigDocument {
  return cloneDocument(profile.defaults);
}

// --- Options --------------------------------------------------------------

// The options worth a row: those with a choice to make.
export function visibleOptions(options: ConfigOption[]): ConfigOption[] {
  return options.filter((o) => o.values.length > 1);
}

// An option's value in the document, else its default.
export function optionValue(doc: ConfigDocument, option: ConfigOption): string {
  return doc.options[option.id] ?? option.default;
}

export function setOption(doc: ConfigDocument, id: string, value: string): ConfigDocument {
  const next = cloneDocument(doc);
  next.options[id] = value;
  return next;
}

// --- Floppies -------------------------------------------------------------

export function setFloppy(doc: ConfigDocument, id: string, type: string): ConfigDocument {
  const next = cloneDocument(doc);
  next.floppies[id] = type;
  return next;
}

// --- Storage --------------------------------------------------------------

export function storageBus(profile: MachineProfile, id: string): StorageBus | undefined {
  return profile.storage.find((b) => b.id === id);
}

// The position text the core composed for (bus, unit).
export function positionLabel(profile: MachineProfile, bus: string, unit: number): string {
  return storageBus(profile, bus)?.units.find((u) => u.unit === unit)?.label ?? `${unit}`;
}

// A device type's label, as the bus names it.
export function deviceTypeLabel(profile: MachineProfile, bus: string, type: string): string {
  return storageBus(profile, bus)?.accepts.find((a) => a.id === type)?.label ?? type;
}

// The buses that share one ID space with `bus` (itself included).
function idSpace(profile: MachineProfile, bus: string): string[] {
  const out = new Set([bus]);
  for (const b of profile.storage) {
    if (b.id === bus) b.shares_units_with.forEach((s) => out.add(s));
    if (b.shares_units_with.includes(bus)) out.add(b.id);
  }
  return [...out];
}

// The units of `bus` a device may take: every unit not used by another
// device on that bus or on a bus sharing its ID space.  `except` is the
// index of a device being moved, whose own unit stays available.
export function freeUnits(
  profile: MachineProfile,
  doc: ConfigDocument,
  bus: string,
  except = -1,
): StorageUnit[] {
  const b = storageBus(profile, bus);
  if (!b) return [];
  const space = idSpace(profile, bus);
  const used = new Set(
    doc.storage.filter((d, i) => i !== except && space.includes(d.bus)).map((d) => d.unit),
  );
  return b.units.filter((u) => !used.has(u.unit));
}

// Where a newly added device of `type` goes on `bus`: the unit the default
// configuration gives that type if free, else the lowest free bay, else the
// lowest free unit.  Null when the bus is full.
export function defaultUnit(
  profile: MachineProfile,
  doc: ConfigDocument,
  bus: string,
  type: string,
): number | null {
  const free = freeUnits(profile, doc, bus);
  if (!free.length) return null;
  const preferred = profile.defaults.storage.find((d) => d.bus === bus && d.type === type);
  if (preferred && free.some((u) => u.unit === preferred.unit)) return preferred.unit;
  const bays = storageBus(profile, bus)?.bays ?? [];
  const bay = free.find((u) => bays.some((y) => y.unit === u.unit));
  return (bay ?? free[0]).unit;
}

// Add a device; the startup device stays where it was (or, with none yet,
// becomes the first hard disk).
export function addDevice(
  profile: MachineProfile,
  doc: ConfigDocument,
  bus: string,
  type: string,
  unit?: number,
): ConfigDocument {
  const at = unit ?? defaultUnit(profile, doc, bus, type);
  if (at === null) return doc;
  const next = cloneDocument(doc);
  next.storage.push({ bus, unit: at, type });
  if (!next.startup) next.startup = firstStartupDevice(profile, next);
  return next;
}

// Remove device `index`.  Removing the startup device moves the choice to
// the first remaining hard disk, else to "no default".
export function removeDevice(
  profile: MachineProfile,
  doc: ConfigDocument,
  index: number,
): ConfigDocument {
  const next = cloneDocument(doc);
  const [gone] = next.storage.splice(index, 1);
  if (gone && next.startup && next.startup.bus === gone.bus && next.startup.unit === gone.unit)
    next.startup = firstStartupDevice(profile, next);
  return next;
}

// Move device `index` to another unit of its bus.
export function setDeviceUnit(doc: ConfigDocument, index: number, unit: number): ConfigDocument {
  const next = cloneDocument(doc);
  const d = next.storage[index];
  if (!d) return doc;
  const wasStartup = !!next.startup && next.startup.bus === d.bus && next.startup.unit === d.unit;
  d.unit = unit;
  if (wasStartup) next.startup = { bus: d.bus, unit };
  return next;
}

// The devices the startup record can name: hard disks and CD-ROM drives on
// a bus whose record can name them.
export function startupChoices(
  profile: MachineProfile,
  doc: ConfigDocument,
): Array<{ bus: string; unit: number; label: string }> {
  return doc.storage
    .filter((d) => storageBus(profile, d.bus)?.startup)
    .map((d) => ({
      bus: d.bus,
      unit: d.unit,
      label: `${deviceTypeLabel(profile, d.bus, d.type)} · ${storageLabel(profile, d)}`,
    }));
}

// A device's place as a sentence fragment: "SCSI ID 0 · Internal hard disk
// bay", the bus named only when the machine has several.
export function storageLabel(profile: MachineProfile, d: StorageDevice): string {
  const pos = positionLabel(profile, d.bus, d.unit);
  if (profile.storage.length < 2) return pos;
  return `${storageBus(profile, d.bus)?.label ?? d.bus} ${pos}`;
}

// The first hard disk the startup record can name, else null ("no default").
function firstStartupDevice(
  profile: MachineProfile,
  doc: ConfigDocument,
): { bus: string; unit: number } | null {
  const hd = doc.storage.find((d) => d.type === 'hd' && storageBus(profile, d.bus)?.startup);
  return hd ? { bus: hd.bus, unit: hd.unit } : null;
}

export function setStartup(
  doc: ConfigDocument,
  startup: { bus: string; unit: number } | null,
): ConfigDocument {
  const next = cloneDocument(doc);
  next.startup = startup;
  return next;
}

// --- Expansion cards -----------------------------------------------------

export function card(profile: MachineProfile, id: string): Card | undefined {
  return profile.cards.find((c) => c.id === id);
}

export function slotLabel(profile: MachineProfile, id: string): string {
  return profile.slots.find((s) => s.id === id)?.label ?? id;
}

// Slots no card occupies and no occupied slot excludes.
function slotIsFree(profile: MachineProfile, cards: CardEntry[], slotId: string): boolean {
  if (cards.some((c) => c.slot === slotId)) return false;
  const slot = profile.slots.find((s) => s.id === slotId);
  if (!slot) return false;
  return !cards.some((c) => slot.excludes.includes(c.slot));
}

// The free slots card `id` fits, in Apple's fill order.
export function freeSlotsFor(
  profile: MachineProfile,
  doc: ConfigDocument,
  id: string,
): Array<{ id: string; label: string }> {
  const c = card(profile, id);
  if (!c) return [];
  return profile.slots
    .filter((s) => c.fits.includes(s.id) && slotIsFree(profile, doc.cards, s.id))
    .sort((a, b) => a.fill_order - b.fill_order)
    .map((s) => ({ id: s.id, label: s.label }));
}

// The cards that could be added now: every card with a free slot it fits.
export function addableCards(profile: MachineProfile, doc: ConfigDocument): Card[] {
  return profile.cards.filter((c) => freeSlotsFor(profile, doc, c.id).length > 0);
}

// Add card `id`, in `slot` or the first free slot it fits.  A display card
// joins the display devices unplugged: adding a card never moves the
// monitor (D6) -- unless nothing has it, when the new card gets it.
export function addCard(
  profile: MachineProfile,
  doc: ConfigDocument,
  id: string,
  slot?: string,
): ConfigDocument {
  const c = card(profile, id);
  const target = slot ?? freeSlotsFor(profile, doc, id)[0]?.id;
  if (!c || !target) return doc;
  const next = cloneDocument(doc);
  const options: Record<string, string> = {};
  for (const o of c.options) options[o.id] = o.default;
  next.cards.push({ slot: target, card: id, options });
  if (c.class === 'display') {
    const nobody = connectedDevice(next) === null;
    next.displays[target] = { monitor: nobody && c.monitors[0] ? c.monitors[0] : NO_MONITOR };
  }
  return next;
}

// Remove the card in `slot`.  If it had the monitor, the monitor moves to
// the first remaining display device.
export function removeCard(
  profile: MachineProfile,
  doc: ConfigDocument,
  slot: string,
): ConfigDocument {
  const next = cloneDocument(doc);
  const wasConnected = connectedDevice(next) === slot;
  next.cards = next.cards.filter((c) => c.slot !== slot);
  delete next.displays[slot];
  if (wasConnected) {
    const first = displayDevices(profile, next)[0];
    if (first) return connectTo(profile, next, first.id);
  }
  return next;
}

export function setCardOption(
  doc: ConfigDocument,
  slot: string,
  key: string,
  value: string,
): ConfigDocument {
  const next = cloneDocument(doc);
  const entry = next.cards.find((c) => c.slot === slot);
  if (entry) entry.options[key] = value;
  return next;
}

// --- Displays ------------------------------------------------------------

// One display device of a configuration: built-in video, or a display card.
export interface DisplayDevice {
  id: string; // "builtin" or the card's slot id
  label: string; // "Built-in video", "Macintosh Display Card 8•24 (NuBus slot 1)"
  detail?: string;
  monitors: string[];
  modes: Record<string, Choice[]>;
  options: ConfigOption[];
  defaultMonitor: string;
}

export function displayDevices(profile: MachineProfile, doc: ConfigDocument): DisplayDevice[] {
  const out: DisplayDevice[] = [];
  const b = profile.displays.builtin;
  if (b)
    out.push({
      id: BUILTIN,
      label: b.label,
      detail: b.detail,
      monitors: b.monitors,
      modes: b.modes,
      options: b.options,
      defaultMonitor: b.default_monitor,
    });
  for (const entry of doc.cards) {
    const c = card(profile, entry.card);
    if (!c || c.class !== 'display') continue;
    out.push({
      id: entry.slot,
      label: `${c.label} (${slotLabel(profile, entry.slot)})`,
      detail: c.detail,
      monitors: c.monitors,
      modes: c.modes,
      options: c.options,
      defaultMonitor: c.monitors[0] ?? NO_MONITOR,
    });
  }
  return out;
}

// The device the monitor is plugged into, or null (no screen).
export function connectedDevice(doc: ConfigDocument): string | null {
  for (const [id, d] of Object.entries(doc.displays)) if (d.monitor !== NO_MONITOR) return id;
  return null;
}

// Plug the monitor into `device`: it keeps the monitor it had when the new
// device takes it, else the device's default; every other device unplugged.
export function connectTo(
  profile: MachineProfile,
  doc: ConfigDocument,
  device: string,
): ConfigDocument {
  const devices = displayDevices(profile, doc);
  const target = devices.find((d) => d.id === device);
  if (!target) return doc;
  const current = connectedDevice(doc);
  const carried = current ? doc.displays[current]?.monitor : undefined;
  const monitor =
    carried && target.monitors.includes(carried) ? carried : target.defaultMonitor || NO_MONITOR;
  const next = cloneDocument(doc);
  next.displays = {};
  for (const d of devices)
    next.displays[d.id] = { monitor: d.id === device ? monitor : NO_MONITOR };
  return next;
}

export function setMonitor(doc: ConfigDocument, device: string, monitor: string): ConfigDocument {
  const next = cloneDocument(doc);
  next.displays[device] = { monitor };
  return next;
}

export function setMode(doc: ConfigDocument, device: string, mode: string): ConfigDocument {
  const next = cloneDocument(doc);
  const d = next.displays[device] ?? { monitor: NO_MONITOR };
  next.displays[device] = mode ? { ...d, mode } : { monitor: d.monitor };
  return next;
}

// --- Before a boot -------------------------------------------------------

// The key an image is chosen under: a storage position.
export function positionKey(d: { bus: string; unit: number }): string {
  return `${d.bus}:${d.unit}`;
}

// A hard disk with no image is dropped before the boot (D15) -- never a
// reason not to start -- and the startup device moves off it.  Returns the
// document to boot and the devices dropped, for the notice.
export function dropImagelessDisks(
  profile: MachineProfile,
  doc: ConfigDocument,
  images: Record<string, string>,
): { doc: ConfigDocument; dropped: StorageDevice[] } {
  let next = cloneDocument(doc);
  const dropped: StorageDevice[] = [];
  for (let i = next.storage.length - 1; i >= 0; i--) {
    const d = next.storage[i];
    if (d.type === 'hd' && !images[positionKey(d)]) {
      dropped.unshift(d);
      next = removeDevice(profile, next, i);
    }
  }
  return { doc: next, dropped };
}

// Warnings the dialog shows; none of them blocks Start.
export function configWarnings(profile: MachineProfile, doc: ConfigDocument): string[] {
  const out: string[] = [];
  if (displayDevices(profile, doc).length === 0 && profile.slots.length > 0)
    out.push('This machine has no built-in video. It will start with no screen.');
  return out;
}
