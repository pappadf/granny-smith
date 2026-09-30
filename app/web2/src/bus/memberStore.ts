// The model's members, shared by the views that show them (the SYSTEM tab,
// the command browser): one cache of each node's structure (meta.members
// without values, keyed by path), one rule for what a core event or a
// finished console job changes, and one subscription that tells the views.
// Values are never cached: meta.members with values is asked each time.

import { onCoreEvent } from './emulator';
import { loadMembers, type MemberInfo } from './systemTree';
import { onConsoleJobDone } from '@/state/console.svelte';
import { forgetUsage } from '@/lib/usage';

export function join(path: string, name: string): string {
  return path ? `${path}.${name}` : name;
}

export const DOMAIN_LABEL: Record<string, string> = {
  machine: 'Machine',
  emulator: 'Emulator',
  network: 'Network',
};

// Whether `path` is `prefix` or lies under it ('' covers everything).
export function covers(prefix: string, path: string): boolean {
  return (
    !prefix || path === prefix || path.startsWith(`${prefix}.`) || path.startsWith(`${prefix}[`)
  );
}

// --- structure ------------------------------------------------------------------

const structure = new Map<string, MemberInfo[]>();

// `path`'s members without values, from the cache or one meta.members call.
// `fresh` skips the cache (and refills it).
export async function members(path: string, fresh = false): Promise<MemberInfo[]> {
  if (!fresh) {
    const hit = structure.get(path);
    if (hit) return hit;
  }
  const ms = await loadMembers(path);
  structure.set(path, ms);
  return ms;
}

// `path`'s members with their values; a node whose values do not fit the
// reply (or fail to read) comes back without them.
export async function membersWithValues(path: string): Promise<MemberInfo[]> {
  const ms = await loadMembers(path, true);
  return ms.length ? ms : members(path);
}

// Drop cached structure at or under `prefix` ('' drops everything, and
// the usage texts with it: the machine changed).
export function invalidate(prefix = ''): void {
  if (!prefix) {
    structure.clear();
    forgetUsage();
    return;
  }
  for (const k of [...structure.keys()]) if (covers(prefix, k)) structure.delete(k);
}

// A console job may add or remove entries anywhere: drop every cached
// level that lists a collection (the containers, and the parents of bare
// indexed members).  Answers the dropped paths.
export function invalidateCollections(): string[] {
  const dropped: string[] = [];
  for (const [path, ms] of [...structure.entries()]) {
    if (ms.some((m) => m.kind === 'child' && (m.indexed || m.collection))) {
      structure.delete(path);
      dropped.push(path);
    }
  }
  return dropped;
}

// A collection's live entries: those of its `entries` member, or -- for a
// bare indexed member reached directly -- those its parent lists for it.
// A hybrid collection (indexed and keyed) is written in the indexed form.
export interface CollectionEntries {
  entries: MemberInfo | undefined; // the container's `entries` member
  items: Array<{ name: string; path: string; word: string }>;
}

export async function collectionEntries(
  path: string,
  ms: MemberInfo[],
): Promise<CollectionEntries> {
  const entries = ms.find((m) => m.kind === 'child' && m.name === 'entries' && m.indexed);
  let source: MemberInfo | undefined = entries;
  if (!source) {
    const dot = path.lastIndexOf('.');
    const parent = dot >= 0 ? path.slice(0, dot) : '';
    const name = dot >= 0 ? path.slice(dot + 1) : path;
    source = (await members(parent, true)).find((m) => m.name === name);
  }
  const indices = Array.isArray(source?.indices) ? source.indices : [];
  const keys = Array.isArray(source?.keys) ? source.keys : [];
  const items = indices.length
    ? indices.map((i) => ({ name: `[${i}]`, path: `${path}[${i}]`, word: String(i) }))
    : keys.map((k) => ({ name: `["${k}"]`, path: `${path}["${k}"]`, word: k }));
  return { entries, items };
}

// --- changes --------------------------------------------------------------------

// What changed in the model.  Every change may have changed values.
export interface MembersChange {
  // Everything (a machine booted): all structure was dropped.
  reload: boolean;
  // Structure dropped at and under these paths.
  dropped: string[];
}

// The core events that change what the views show, and the structure each
// drops (null: values only).  The machine booting drops everything.
const EVENT_DROPS: Record<string, string | null> = {
  'state:machine_booted': '',
  'notify:floppy': 'machine.floppy',
  'notify:media': 'machine.scsi',
  'notify:checkpoint_saved': 'checkpoint',
  'state:mode_started': null,
  'state:mode_ended': null,
  'state:breakpoint_hit': null,
  'state:speed': null,
  'notify:drive_activity': null,
};

// The change a core event (`kind:event`) makes, or null when it changes
// nothing shown.
export function changeFor(event: string): MembersChange | null {
  if (!(event in EVENT_DROPS)) return null;
  const drop = EVENT_DROPS[event];
  if (drop === '') return { reload: true, dropped: [] };
  return { reload: false, dropped: drop === null ? [] : [drop] };
}

type Listener = (change: MembersChange) => void;
const listeners = new Set<Listener>();
let unhook: (() => void) | null = null;

function emit(change: MembersChange): void {
  for (const l of [...listeners]) l(change);
}

function hook(): () => void {
  const offEvents = onCoreEvent((ev) => {
    const change = changeFor(`${ev.kind}:${ev.event}`);
    if (!change) return;
    if (change.reload) invalidate('');
    for (const p of change.dropped) invalidate(p);
    emit(change);
  });
  const offJobs = onConsoleJobDone(() => emit({ reload: false, dropped: invalidateCollections() }));
  return () => {
    offEvents();
    offJobs();
  };
}

// Call `cb` on every change, after the cache has dropped what it changed.
// Answers the unsubscribe.
export function onMembersChanged(cb: Listener): () => void {
  listeners.add(cb);
  if (!unhook) unhook = hook();
  return () => {
    listeners.delete(cb);
    if (!listeners.size && unhook) {
      unhook();
      unhook = null;
    }
  };
}
