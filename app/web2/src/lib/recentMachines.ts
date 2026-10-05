// The Welcome page's Recent list: machines started in this browser, newest
// first, each the exact input initEmulator took (the machine.boot document —
// model id, ROM path, configuration — and the media attached after it), so a
// relaunch is the same call and cannot drift from what machine.boot takes.
// Pure helpers; the state and its persistence are state/recent.svelte.ts and
// state/persist.svelte.ts.

import type { MachineConfig, OpfsEntry } from '@/bus/types';

// One started machine.
export interface RecentMachine {
  /** What initEmulator was called with: the boot document plus its media. */
  config: MachineConfig;
  /** Readable summary ("Macintosh IIcx · 8 MB · 8•24 GC · System_7_1.img"). */
  label: string;
  /** When it was last started (ms since the epoch). */
  lastUsed: number;
}

// How many machines the list keeps.
export const RECENT_MAX = 8;

// Canonical JSON: object keys sorted at every level, so two configurations
// that differ only in key order are one entry.
function canonical(v: unknown): string {
  if (Array.isArray(v)) return `[${v.map(canonical).join(',')}]`;
  if (v && typeof v === 'object') {
    const o = v as Record<string, unknown>;
    const keys = Object.keys(o)
      .filter((k) => o[k] !== undefined)
      .sort();
    return `{${keys.map((k) => `${JSON.stringify(k)}:${canonical(o[k])}`).join(',')}}`;
  }
  return JSON.stringify(v);
}

// The identity of a configuration: equal keys boot the same machine.
export function recentKey(config: MachineConfig): string {
  return canonical(config);
}

// The list with `entry` on top: an identical configuration already in it
// moves up (taking the new label and time), and the list keeps `max`.
export function pushRecent(
  list: readonly RecentMachine[],
  entry: RecentMachine,
  max: number = RECENT_MAX,
): RecentMachine[] {
  const key = recentKey(entry.config);
  return [entry, ...list.filter((e) => recentKey(e.config) !== key)].slice(0, max);
}

// The list without the configuration whose key is `key`.
export function forgetRecent(list: readonly RecentMachine[], key: string): RecentMachine[] {
  return list.filter((e) => recentKey(e.config) !== key);
}

// A persisted list as read back: anything malformed is dropped, not trusted.
export function parseRecent(raw: unknown, max: number = RECENT_MAX): RecentMachine[] {
  if (!Array.isArray(raw)) return [];
  const out: RecentMachine[] = [];
  for (const e of raw) {
    if (!e || typeof e !== 'object') continue;
    const { config, label, lastUsed } = e as Partial<RecentMachine>;
    if (!config || typeof config !== 'object' || typeof config.model !== 'string') continue;
    if (typeof label !== 'string' || typeof lastUsed !== 'number') continue;
    out.push({ config, label, lastUsed });
  }
  return out.slice(0, max);
}

// Every file a configuration needs: the ROM, then the floppy, hard disk and
// CD-ROM images.
export function referencedFiles(config: MachineConfig): string[] {
  const out: string[] = [];
  if (config.rom) out.push(config.rom);
  for (const p of Object.values(config.floppies ?? {})) if (p) out.push(p);
  for (const m of config.media ?? []) if (m.path) out.push(m.path);
  return out;
}

// The last path segment ("System_7_1.img").
export function baseName(path: string): string {
  return path.split('/').pop() ?? path;
}

// The row's label: the machine's name, its RAM, its slot cards and the
// first disk image (a hard disk or CD before a floppy), joined with " · ".
export function recentLabel(parts: {
  name: string;
  ram?: string | null;
  cards?: string[];
  config: MachineConfig;
}): string {
  const disk = parts.config.media?.[0]?.path ?? Object.values(parts.config.floppies ?? {})[0];
  return [parts.name, parts.ram, ...(parts.cards ?? []), disk ? baseName(disk) : null]
    .filter((s): s is string => !!s)
    .join(' · ');
}

// For each entry (by recentKey) whose ROM or an image is gone, the first
// missing file's name.  `list` is the storage's directory listing (opfs.list);
// each directory is listed once.  A path outside /opfs is not OPFS's to
// answer for and counts as present.
export async function findMissingFiles(
  entries: readonly RecentMachine[],
  list: (dir: string) => Promise<OpfsEntry[]>,
): Promise<Record<string, string>> {
  const listings = new Map<string, Promise<Set<string>>>();
  const filesIn = (dir: string) => {
    let p = listings.get(dir);
    if (!p) {
      p = list(dir)
        .then((es) => new Set(es.filter((e) => e.kind === 'file').map((e) => e.name)))
        .catch(() => new Set<string>());
      listings.set(dir, p);
    }
    return p;
  };
  const out: Record<string, string> = {};
  for (const e of entries) {
    for (const path of referencedFiles(e.config)) {
      if (!path.startsWith('/opfs/')) continue;
      const dir = path.slice(0, path.lastIndexOf('/'));
      if (!(await filesIn(dir)).has(baseName(path))) {
        out[recentKey(e.config)] = baseName(path);
        break;
      }
    }
  }
  return out;
}

const RELATIVE = new Intl.RelativeTimeFormat('en', { numeric: 'auto' });
// Unit thresholds, largest first: [unit, its length in seconds].
const UNITS: [Intl.RelativeTimeFormatUnit, number][] = [
  ['year', 365 * 86400],
  ['month', 30 * 86400],
  ['week', 7 * 86400],
  ['day', 86400],
  ['hour', 3600],
  ['minute', 60],
];

// "just now", "5 minutes ago", "2 hours ago", "yesterday", "3 weeks ago".
export function formatRelativeTime(then: number, now: number = Date.now()): string {
  const secs = Math.max(0, (now - then) / 1000);
  for (const [unit, len] of UNITS) {
    if (secs >= len) return RELATIVE.format(-Math.floor(secs / len), unit);
  }
  return 'just now';
}
