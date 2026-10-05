// The SYSTEM tab, as data: the model's state, level by level.  The root's
// children sit under their domain dividers, in model order; a node's
// attributes (with values) and children follow in model order; a collection
// expands to its live entries.  A node that has only methods (no attributes,
// no children, e.g. `memory` with `peek`) is not a folder: its methods are a
// submenu of its parent's context menu.  Everything comes from meta.members;
// nothing is hand-maintained.

import { isModuleReady } from '@/bus/emulator';
import type { MemberInfo, TypeDescriptor } from '@/bus/systemTree';
import {
  DOMAIN_LABEL,
  collectionEntries,
  join,
  members,
  membersWithValues,
} from '@/bus/memberStore';

export type SysKind = 'divider' | 'object' | 'collection' | 'entry' | 'attr';

export interface SysRow {
  key: string; // the path, or divider:<domain>
  kind: SysKind;
  name: string; // the path segment (`d0`, `drive`, `[0]`, `["scsi"]`)
  label: string; // what the row shows (a node's model label, else its name)
  path: string;
  doc: string;
  category: string; // basic | advanced | internal
  expandable: boolean;
  type?: TypeDescriptor; // attr
  readonly?: boolean; // attr
  value?: unknown; // attr: its tagged value (undefined when not read)
}

// A methods-only child, shown as a submenu of its parent's menu.
export interface Submenu {
  name: string;
  path: string;
  methods: MemberInfo[];
}

export interface Level {
  rows: SysRow[];
  // The node's own methods (the context menu), hidden ones dropped.
  methods: MemberInfo[];
  submenus: Submenu[];
}

const EMPTY: Level = { rows: [], methods: [], submenus: [] };

function visibleMethods(ms: MemberInfo[]): MemberInfo[] {
  return ms.filter((m) => m.kind === 'method' && !m.hidden && m.category !== 'internal');
}

function isMethodsOnly(ms: MemberInfo[]): boolean {
  return (
    ms.some((m) => m.kind === 'method' && !m.hidden) &&
    !ms.some((m) => m.kind === 'attr' || m.kind === 'child')
  );
}

function attrRow(path: string, m: MemberInfo): SysRow {
  const full = join(path, m.name);
  return {
    key: full,
    kind: 'attr',
    name: m.name,
    label: m.name,
    path: full,
    doc: m.doc ?? '',
    category: m.category ?? 'basic',
    expandable: false,
    type: m.type,
    readonly: !!m.readonly,
    value: m.value,
  };
}

function nodeRow(path: string, m: MemberInfo): SysRow {
  const full = join(path, m.name);
  const collection = !!m.collection || !!m.indexed;
  return {
    key: full,
    kind: collection ? 'collection' : 'object',
    name: m.name,
    label: m.label && m.label !== m.name ? m.label : m.name,
    path: full,
    doc: m.doc ?? '',
    category: m.category ?? 'basic',
    expandable: true,
  };
}

function entryRow(name: string, path: string): SysRow {
  return {
    key: path,
    kind: 'entry',
    name,
    label: name,
    path,
    doc: '',
    category: 'basic',
    expandable: true,
  };
}

function dividerRow(domain: string): SysRow {
  return {
    key: `divider:${domain}`,
    kind: 'divider',
    name: DOMAIN_LABEL[domain] ?? domain,
    label: DOMAIN_LABEL[domain] ?? domain,
    path: '',
    doc: '',
    category: 'basic',
    expandable: false,
  };
}

// Rows for a node's members, in model order; methods-only children become
// submenus.
async function memberRows(path: string, ms: MemberInfo[], skip?: MemberInfo): Promise<Level> {
  const children = ms.filter(
    (m) => m !== skip && m.kind === 'child' && !m.collection && !m.indexed,
  );
  const kinds = await Promise.all(
    children.map(async (m) => isMethodsOnly(await members(join(path, m.name)))),
  );
  const methodsOnly = new Set(children.filter((_, i) => kinds[i]));
  const rows: SysRow[] = [];
  const submenus: Submenu[] = [];
  for (const m of ms) {
    if (m === skip) continue;
    if (m.kind === 'attr') rows.push(attrRow(path, m));
    else if (m.kind === 'child') {
      if (methodsOnly.has(m)) {
        const sub = join(path, m.name);
        submenus.push({ name: m.name, path: sub, methods: visibleMethods(await members(sub)) });
      } else rows.push(nodeRow(path, m));
    }
  }
  return { rows, methods: visibleMethods(ms), submenus };
}

// The root: its children under domain dividers.
export async function loadRoot(): Promise<Level> {
  if (!isModuleReady()) return EMPTY;
  const ms = await members('');
  const rows: SysRow[] = [];
  let domain: string | undefined;
  for (const m of ms) {
    if (m.kind !== 'child' || m.category === 'internal') continue;
    const d = m.domain ?? 'emulator';
    if (d !== domain) {
      domain = d;
      rows.push(dividerRow(d));
    }
    rows.push(nodeRow('', m));
  }
  return { rows, methods: [], submenus: [] };
}

// The level under an object or entry row.
async function objectLevel(path: string): Promise<Level> {
  return memberRows(path, await membersWithValues(path));
}

// The level under a collection: its live entries (by index, or by key), then
// the container's own members except `entries`.
async function collectionLevel(path: string): Promise<Level> {
  const ms = await membersWithValues(path);
  const { entries, items } = await collectionEntries(path, ms);
  const rows = items.map((e) => entryRow(e.name, e.path));
  // A bare indexed member answers for itself only through its entries.
  if (!entries) return { rows, methods: [], submenus: [] };
  const own = await memberRows(path, ms, entries);
  return { rows: [...rows, ...own.rows], methods: own.methods, submenus: own.submenus };
}

// The level under `row`.
export async function loadLevel(row: Pick<SysRow, 'kind' | 'path'>): Promise<Level> {
  if (!isModuleReady()) return EMPTY;
  switch (row.kind) {
    case 'collection':
      return collectionLevel(row.path);
    case 'object':
    case 'entry':
      return objectLevel(row.path);
    default:
      return EMPTY;
  }
}

// Whether a row is shown: never internal; advanced only with the toggle.
export function shown(row: SysRow, showAdvanced: boolean): boolean {
  if (row.category === 'internal') return false;
  return row.category !== 'advanced' || showAdvanced;
}

export const REFRESH_INTERVAL_MS = 2000;
