// A URL boot's configuration parameters: edits to the configuration document.
//
// Besides its media, a URL that boots a machine can choose what the
// configuration dialog chooses -- any scalar option of the model's tree, and
// the screen -- by name:
//
//   ?rom=…&hd0=…&addressing=32&memory=32768&monitor=21in_rgb&mode=1152x870x8
//
// Each parameter is an edit applied with the dialog's own editors
// (lib/machineConfig.ts) to the document the boot would otherwise use (the
// URL's config=, else the model's default configuration), and machine.boot
// validates the result like any document.  Nothing here knows a machine: an
// option parameter is any option id the model's tree declares, matched
// against that tree's values, so an option the core adds is a URL parameter
// with no change here.
//
// - <option id>=<value>: a scalar option (memory, appletalk, addressing,
//   power_supplies, …).  The value is one of the option's value ids, or its
//   label ignoring case and spaces (`32-bit`, `32MB`).  `ram=` is `memory=`.
// - display=<device>: the display device the monitor is plugged into
//   (`builtin`, `nubus_9`, `pci_1`), on a machine with more than one.
// - monitor=<id>: the monitor on that device (`13in_rgb`, `21in_rgb`).
// - mode=<W>x<H>x<D> (or <W>x<H>, the deepest mode at that size): the
//   startup video mode.  Without monitor=, the device's monitors are searched
//   for it, the plugged-in one first.
//
// A parameter that names nothing in the tree, or a value it does not offer,
// is reported and left out: the machine boots without that edit.

import type { ConfigDocument, ConfigOption, MachineProfile } from '@/bus/profile';
import * as mc from './machineConfig';

// Names the page reads for something else: the media and boot parameters
// (bus/urlMedia.ts), the skin, and the component gallery's.  They are never
// option names; a core option id that collided with one could not be set
// from a URL (tests/unit/urlConfig.test.ts keeps the trees clear of them).
const PAGE_PARAM =
  /^(rom|vrom|model|speed|cd|config|skin|gallery|story|variant|motion|view|(fd|hd)\d*)$/;

// The display edits' names.
const DISPLAY_PARAMS = ['display', 'monitor', 'mode'] as const;

// Other spellings of an option id.
const OPTION_ALIASES: Record<string, string> = { ram: 'memory' };

// The configuration edits a URL asks for, in the order given; names are
// lower case, aliases resolved.
export interface UrlConfigParams {
  options: Array<{ id: string; value: string; name: string }>;
  display: string | null;
  monitor: string | null;
  mode: string | null;
}

export function emptyUrlConfigParams(): UrlConfigParams {
  return { options: [], display: null, monitor: null, mode: null };
}

export function hasUrlConfigParams(p: UrlConfigParams): boolean {
  return p.options.length > 0 || !!(p.display || p.monitor || p.mode);
}

// Whether `name` (lower case) is a configuration parameter: not one the page
// reads for anything else.  Which option ids exist is the model's tree's to
// say, once the ROM has chosen the model (applyUrlConfig).
export function isUrlConfigParam(name: string): boolean {
  return !PAGE_PARAM.test(name);
}

// Record one configuration parameter (the first spelling of a name wins).
export function addUrlConfigParam(p: UrlConfigParams, rawName: string, value: string): void {
  const name = rawName.toLowerCase();
  if ((DISPLAY_PARAMS as readonly string[]).includes(name)) {
    const key = name as (typeof DISPLAY_PARAMS)[number];
    if (p[key] === null) p[key] = value;
    return;
  }
  const id = OPTION_ALIASES[name] ?? name;
  if (p.options.some((o) => o.id === id)) {
    console.warn(`[urlConfig] ignoring ${rawName}=: ${id} is already set`);
    return;
  }
  p.options.push({ id, value, name: rawName });
}

// Lower case, without spaces, so "32 MB" is "32mb".
function squash(s: string): string {
  return s.toLowerCase().replace(/\s+/g, '');
}

// The value id of `option` that `value` names, or null.
function optionValueId(option: ConfigOption, value: string): string | null {
  const v = value.trim();
  const byId = option.values.find((c) => c.id.toLowerCase() === v.toLowerCase());
  if (byId) return byId.id;
  const byLabel = option.values.find((c) => squash(c.label) === squash(v));
  return byLabel ? byLabel.id : null;
}

// "1152x870x8" → [1152, 870, 8]; "1152x870" → [1152, 870, null]; else null.
function parseMode(s: string): [number, number, number | null] | null {
  const m = /^\s*(\d+)\s*[x×]\s*(\d+)\s*(?:[x×]\s*(\d+))?\s*$/i.exec(s);
  if (!m) return null;
  return [Number(m[1]), Number(m[2]), m[3] === undefined ? null : Number(m[3])];
}

// The mode id among `ids` that `want` names: the exact one, or for a size
// alone the deepest at that size.
function pickMode(ids: string[], want: [number, number, number | null]): string | null {
  let best: string | null = null;
  let bestDepth = -1;
  for (const id of ids) {
    const m = parseMode(id);
    if (!m || m[0] !== want[0] || m[1] !== want[1] || m[2] === null) continue;
    if (want[2] !== null) {
      if (m[2] === want[2]) return id;
    } else if (m[2] > bestDepth) {
      best = id;
      bestDepth = m[2];
    }
  }
  return best;
}

// A document the edits can work on: the default configuration, or `base` (a
// URL's config=, which may leave nodes out to mean "the default") with just
// the nodes the edits read filled in -- options, and the cards and displays
// the screen edits look at.  Every other node it leaves out stays out, so
// the core still derives it from what `base` did say (its startup device
// from its storage); displays are filled only along with the cards, so a
// config= that chose its own cards keeps "displays follow the cards".
function editable(profile: MachineProfile, base: Record<string, unknown> | null): ConfigDocument {
  const defaults = mc.defaultDocument(profile);
  if (!base) return defaults;
  const doc = JSON.parse(JSON.stringify(base)) as Record<string, unknown>;
  doc.options ??= {};
  if (!('cards' in doc)) {
    doc.cards = defaults.cards;
    doc.displays ??= defaults.displays;
  }
  doc.displays ??= {};
  return doc as unknown as ConfigDocument;
}

// Apply a URL's configuration parameters to the document the boot would use
// (`base`: config= or the floppy fix-up's document, null for the default).
// Returns the document to boot (`base` itself when nothing applied) and what
// was left out, each as a sentence for the page to show.
export function applyUrlConfig(
  profile: MachineProfile,
  base: Record<string, unknown> | null,
  params: UrlConfigParams,
): { config: Record<string, unknown> | null; warnings: string[] } {
  const warnings: string[] = [];
  if (!hasUrlConfigParams(params)) return { config: base, warnings };
  let doc = editable(profile, base);
  let changed = false;

  // Scalar options: whatever the model's tree declares.
  for (const p of params.options) {
    const option = profile.options.find((o) => o.id === p.id);
    if (!option) {
      // Not this model's: an option another model has, or a parameter for
      // someone else (a tracking tag).  Nothing to say to the user.
      console.warn(`[urlConfig] ${p.name}=: ${profile.name} has no such option`);
      continue;
    }
    const id = optionValueId(option, p.value);
    if (id === null) {
      const values = option.values.map((v) => v.label).join(', ');
      warnings.push(`${p.name}=${p.value}: ${option.label} is one of ${values}`);
      continue;
    }
    doc = mc.setOption(doc, option.id, id);
    changed = true;
  }

  // The screen: which device, which monitor, which startup mode.
  if (params.display || params.monitor || params.mode) {
    const devices = mc.displayDevices(profile, doc);
    let deviceId = mc.connectedDevice(doc);
    if (params.display) {
      const want = params.display.toLowerCase();
      const dev = devices.find((d) => d.id.toLowerCase() === want);
      if (!dev) {
        const ids = devices.map((d) => d.id).join(', ');
        warnings.push(`display=${params.display}: this configuration's displays are ${ids}`);
      } else if (dev.id !== deviceId) {
        doc = mc.connectTo(profile, doc, dev.id);
        deviceId = dev.id;
        changed = true;
      }
    }
    if (!deviceId && devices.length > 0 && (params.monitor || params.mode)) {
      doc = mc.connectTo(profile, doc, devices[0].id);
      deviceId = devices[0].id;
      changed = true;
    }
    const device = devices.find((d) => d.id === deviceId);
    if (!device) {
      if (params.monitor || params.mode)
        warnings.push('monitor= / mode=: this configuration has no display');
    } else {
      const plugged = (): string => doc.displays[device.id]?.monitor ?? mc.NO_MONITOR;
      const monitorLabel = (id: string): string =>
        profile.monitors.find((m) => m.id === id)?.label ?? id;
      let monitorOk = true;
      if (params.monitor) {
        const want = params.monitor.toLowerCase();
        const monitor = device.monitors.find(
          (m) =>
            m !== mc.NO_MONITOR &&
            (m.toLowerCase() === want || squash(monitorLabel(m)) === squash(params.monitor!)),
        );
        if (!monitor) {
          monitorOk = false;
          const ids = device.monitors.filter((m) => m !== mc.NO_MONITOR).join(', ');
          warnings.push(`monitor=${params.monitor}: ${device.label} takes ${ids}`);
        } else if (monitor !== plugged()) {
          doc = mc.setMonitor(doc, device.id, monitor);
          changed = true;
        }
      }
      if (params.mode) {
        const want = parseMode(params.mode);
        // The monitors to look in: the one asked for, else the plugged-in
        // one first and then the rest of the device's.
        const current = plugged();
        const order = params.monitor
          ? monitorOk
            ? [current]
            : []
          : [current, ...device.monitors.filter((m) => m !== current && m !== mc.NO_MONITOR)];
        let found: { monitor: string; mode: string } | null = null;
        for (const m of want ? order : []) {
          const mode = pickMode(
            (device.modes[m] ?? []).map((c) => c.id),
            want!,
          );
          if (mode) {
            found = { monitor: m, mode };
            break;
          }
        }
        if (!found) {
          const offered = (device.modes[current] ?? []).map((c) => c.id).join(', ');
          warnings.push(
            want
              ? `mode=${params.mode}: ${monitorLabel(current)} on ${device.label} offers ${offered || 'no modes'}`
              : `mode=${params.mode}: a mode is <width>x<height>[x<depth>]`,
          );
        } else {
          if (found.monitor !== current) doc = mc.setMonitor(doc, device.id, found.monitor);
          doc = mc.setMode(doc, device.id, found.mode);
          changed = true;
        }
      }
    }
  }

  return { config: changed ? (doc as unknown as Record<string, unknown>) : base, warnings };
}
