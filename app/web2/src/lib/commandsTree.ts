// The Terminal's command browser, as data: a STRUCTURAL view of the one
// object model -- levels are path segments, exactly as they are typed -- with
// every row generated from meta.members.  Nothing here is hand-maintained:
// the keywords come from shell.keywords, the
// aliases from shell.alias.list, and a row's one-line description, type and
// usage text from the model (docs/internals/core/object/object-model.md).
//
// Levels load lazily and are cached per node path; the cache is invalidated
// by the core events that change what a level holds (invalidateFor).

import { gsEval, isModuleReady } from '@/bus/emulator';
import { loadMembers, type MemberInfo, type TypeDescriptor } from '@/bus/systemTree';

export type RowKind =
  | 'divider' // a non-interactive domain heading
  | 'object'
  | 'collection' // a collection container: its entries are addressed [i] / ["k"]
  | 'entry' // one entry of a collection
  | 'method'
  | 'attr'
  | 'group' // a synthetic grouping (Aliases, Language, their subgroups)
  | 'alias'
  | 'keyword';

export interface BrowserRow {
  key: string; // unique across the tree (the path, or a synthetic id)
  kind: RowKind;
  name: string; // the path segment shown (`drive`, `[0]`, `["scsi"]`, `$pc`)
  path: string; // the full path ('' for synthetic rows)
  doc: string;
  category: string; // basic | advanced | internal
  insert: string; // what selecting the row writes over the path token at the cursor
  expandable: boolean;
  type?: TypeDescriptor; // attr
  readonly?: boolean; // attr
  keyed?: boolean; // collection: its entries are keyed by name
}

// The first sentence of a doc (up to the first ". " or the end).
export function firstSentence(doc: string): string {
  const i = doc.indexOf('. ');
  return i >= 0 ? doc.slice(0, i + 1) : doc;
}

function join(path: string, name: string): string {
  return path ? `${path}.${name}` : name;
}

// --- cache --------------------------------------------------------------------

const memberCache = new Map<string, MemberInfo[]>();

// `path`'s members, from the cache or one meta.members call.  `fresh` skips
// the cache (a collection container is always re-read on expansion).
async function members(path: string, fresh = false): Promise<MemberInfo[]> {
  if (!fresh) {
    const hit = memberCache.get(path);
    if (hit) return hit;
  }
  const ms = await loadMembers(path);
  memberCache.set(path, ms);
  return ms;
}

// Drop cached levels at or under `prefix` ('' drops everything).
export function invalidate(prefix = ''): void {
  if (!prefix) {
    memberCache.clear();
    return;
  }
  for (const k of [...memberCache.keys()])
    if (k === prefix || k.startsWith(`${prefix}.`) || k.startsWith(`${prefix}[`))
      memberCache.delete(k);
}

// A console job may add or remove entries anywhere: drop every cached
// level that lists a collection (the containers, and the parents of bare
// indexed members).  Answers the dropped paths.
export function invalidateCollections(): string[] {
  const dropped: string[] = [];
  for (const [path, ms] of [...memberCache.entries()]) {
    if (ms.some((m) => m.kind === 'child' && (m.indexed || m.collection))) {
      memberCache.delete(path);
      dropped.push(path);
    }
  }
  return dropped;
}

// The cache invalidation a core event implies (`kind:event`), or null when
// it changes nothing the browser shows.
export function invalidationFor(event: string): string | null {
  switch (event) {
    case 'state:machine_booted':
      return '';
    case 'notify:floppy':
      return 'machine.floppy';
    case 'notify:media':
      return 'machine.scsi';
    case 'notify:checkpoint_saved':
      return 'checkpoint';
    default:
      return null;
  }
}

// --- rows ---------------------------------------------------------------------

// The row a member of `path` becomes.
function memberRow(path: string, m: MemberInfo): BrowserRow {
  const full = join(path, m.name);
  const base = {
    key: full,
    name: m.name,
    path: full,
    doc: m.doc ?? '',
    category: m.category ?? 'basic',
  };
  if (m.kind === 'method')
    return { ...base, kind: 'method', insert: `${full} `, expandable: false };
  if (m.kind === 'attr')
    return {
      ...base,
      kind: 'attr',
      insert: full,
      expandable: false,
      type: m.type,
      readonly: !!m.readonly,
    };
  // A child: a collection container (or a bare indexed member), or an object.
  const indexedMember = !!m.indexed;
  const isCollection = !!m.collection || indexedMember;
  // Keyed only: a hybrid (indexed and keyed) collection is written with `[`.
  const keyed =
    isCollection && Array.isArray(m.keys) && !(Array.isArray(m.indices) && m.indices.length);
  return {
    ...base,
    kind: isCollection ? 'collection' : 'object',
    insert: isCollection ? (keyed ? `${full}["` : `${full}[`) : `${full}.`,
    expandable: true,
    keyed,
  };
}

// Whether a member is shown at all: every basic and advanced member, never
// internal ones (hidden methods are left out when the level is read).
export function visible(row: BrowserRow): boolean {
  return row.category !== 'internal';
}

// The rows directly under an object path: its attributes, methods and
// children in model order.
async function objectRows(path: string): Promise<BrowserRow[]> {
  const ms = await members(path);
  return ms.filter((m) => !(m.kind === 'method' && m.hidden)).map((m) => memberRow(path, m));
}

// The rows under a collection container: its live entries (by index, or by
// key), then the container's own members (add, clear, find, …) except the
// `entries` member itself.
async function collectionRows(path: string): Promise<BrowserRow[]> {
  // Re-read on every expansion: entries come and go.
  const ms = await members(path, true);
  const entriesMember = ms.find((m) => m.kind === 'child' && m.name === 'entries' && m.indexed);
  const out: BrowserRow[] = [];
  // Entries of the container (its `entries` member), or -- for a bare indexed
  // member reached directly -- of the member itself, whose parent answers.
  let indices: number[] = [];
  let keys: string[] = [];
  if (entriesMember) {
    indices = Array.isArray(entriesMember.indices) ? entriesMember.indices : [];
    keys = Array.isArray(entriesMember.keys) ? entriesMember.keys : [];
  } else {
    const dot = path.lastIndexOf('.');
    const parent = dot >= 0 ? path.slice(0, dot) : '';
    const name = dot >= 0 ? path.slice(dot + 1) : path;
    const pm = (await members(parent, true)).find((m) => m.name === name);
    indices = Array.isArray(pm?.indices) ? pm.indices : [];
    keys = Array.isArray(pm?.keys) ? pm.keys : [];
  }
  // A hybrid collection (indexed and keyed) is written in the indexed form.
  if (indices.length) {
    for (const i of indices) out.push(entryRow(`[${i}]`, `${path}[${i}]`));
  } else {
    for (const k of keys) out.push(entryRow(`["${k}"]`, `${path}["${k}"]`));
  }
  for (const m of ms) {
    if (m === entriesMember || (m.kind === 'method' && m.hidden)) continue;
    out.push(memberRow(path, m));
  }
  return out;
}

function entryRow(name: string, path: string): BrowserRow {
  return {
    key: path,
    kind: 'entry',
    name,
    path,
    doc: '',
    category: 'basic',
    insert: `${path}.`,
    expandable: true,
  };
}

// The rows under `row`.
export async function childRows(row: BrowserRow): Promise<BrowserRow[]> {
  switch (row.kind) {
    case 'collection':
      return collectionRows(row.path);
    case 'object':
    case 'entry':
      return objectRows(row.path);
    default:
      return [];
  }
}

const DOMAIN_LABEL: Record<string, string> = {
  machine: 'Machine',
  emulator: 'Emulator',
  network: 'Network',
};

// The browser's root: the root's own verbs, then its children under domain
// dividers (in model order), then the Aliases and Language groups.
export async function rootRows(): Promise<BrowserRow[]> {
  if (!isModuleReady()) return [];
  const ms = await members('');
  const out: BrowserRow[] = [];
  for (const m of ms) if (m.kind === 'method' && !m.hidden) out.push(memberRow('', m));
  let domain: string | undefined;
  for (const m of ms) {
    if (m.kind !== 'child') continue;
    const d = m.domain ?? 'emulator';
    if (d !== domain) {
      domain = d;
      out.push(divider(d));
    }
    out.push(memberRow('', m));
  }
  out.push(
    group('group:aliases', 'Aliases', 'Built-in and user $name shortcuts that expand to a path'),
  );
  out.push(group('group:language', 'Language', 'Shell keywords and their syntax'));
  return out;
}

function divider(domain: string): BrowserRow {
  return {
    key: `divider:${domain}`,
    kind: 'divider',
    name: DOMAIN_LABEL[domain] ?? domain,
    path: '',
    doc: '',
    category: 'basic',
    insert: '',
    expandable: false,
  };
}

function group(key: string, name: string, doc: string): BrowserRow {
  return {
    key,
    kind: 'group',
    name,
    path: '',
    doc,
    category: 'basic',
    insert: '',
    expandable: true,
  };
}

// The rows of a synthetic group: Aliases (User, Built-in, and a collapsed Mac
// globals subgroup for the ~500 built-ins over debug.mac.globals) and
// Language (shell.keywords).
export async function groupRows(row: BrowserRow): Promise<BrowserRow[]> {
  if (row.key === 'group:language') {
    const kws = await gsEval('shell.keywords');
    if (!Array.isArray(kws)) return [];
    return kws
      .filter((k): k is { word: string; syntax: string } => !!k && typeof k.word === 'string')
      .map((k) => ({
        key: `kw:${k.word}`,
        kind: 'keyword' as const,
        name: k.word,
        path: '',
        doc: k.syntax,
        category: 'basic',
        insert: `${k.word} `,
        expandable: false,
      }));
  }
  const aliases = await loadAliases();
  if (row.key === 'group:aliases')
    return [
      group('group:aliases:user', 'User', 'Aliases defined in this session'),
      group('group:aliases:builtin', 'Built-in', 'Aliases the shell defines'),
      group(
        'group:aliases:globals',
        'Mac globals',
        'Built-in aliases for the Mac low-memory globals',
      ),
    ];
  const pick =
    row.key === 'group:aliases:user'
      ? aliases.filter((a) => !a.builtin)
      : row.key === 'group:aliases:globals'
        ? aliases.filter((a) => a.builtin && a.path.startsWith('debug.mac.globals.'))
        : aliases.filter((a) => a.builtin && !a.path.startsWith('debug.mac.globals.'));
  return pick.map((a) => ({
    key: `alias:${a.name}`,
    kind: 'alias' as const,
    name: `$${a.name}`,
    path: a.path,
    doc: a.path,
    category: 'basic',
    insert: `$${a.name}`,
    expandable: false,
  }));
}

export interface AliasInfo {
  name: string;
  path: string;
  builtin: boolean;
}

// shell.alias.list entries are `name=path` or `name=path (built-in)`.
export async function loadAliases(): Promise<AliasInfo[]> {
  const list = await gsEval('shell.alias.list');
  if (!Array.isArray(list)) return [];
  const out: AliasInfo[] = [];
  for (const s of list) {
    if (typeof s !== 'string') continue;
    const eq = s.indexOf('=');
    if (eq < 0) continue;
    const rest = s.slice(eq + 1);
    const builtin = rest.endsWith(' (built-in)');
    out.push({
      name: s.slice(0, eq),
      path: builtin ? rest.slice(0, -' (built-in)'.length) : rest,
      builtin,
    });
  }
  return out;
}

// The rows under any expandable row.
export async function expand(row: BrowserRow): Promise<BrowserRow[]> {
  return row.kind === 'group' ? groupRows(row) : childRows(row);
}

// shell.usage: the usage text, and for a method its signature (the text's
// first line) with each declared argument's [start, end) in it (UTF-8
// bytes, as the core counts).
export interface UsageInfo {
  text: string;
  signature: string;
  argSpans: Array<[number, number] | null>;
}

export async function loadUsageInfo(path: string): Promise<UsageInfo | null> {
  if (!path) return null;
  const u = await gsEval('shell.usage', [path]);
  if (!u || typeof u !== 'object' || typeof (u as { text?: unknown }).text !== 'string')
    return null;
  const o = u as { text: string; signature?: unknown; arg_spans?: unknown };
  const spans = Array.isArray(o.arg_spans)
    ? o.arg_spans.map((p) =>
        Array.isArray(p) && typeof p[0] === 'number' && typeof p[1] === 'number'
          ? ([p[0], p[1]] as [number, number])
          : null,
      )
    : [];
  return {
    text: o.text,
    signature: typeof o.signature === 'string' ? o.signature : '',
    argSpans: spans,
  };
}

// The usage text of a leaf (shell.usage), or '' when it has none.
export async function loadUsage(path: string): Promise<string> {
  return (await loadUsageInfo(path))?.text ?? '';
}

// Type text of an attribute row: kind plus hex / bin / path, as usage shows it.
export function typeText(t?: TypeDescriptor): string {
  if (!t) return '';
  if (t.kind === 'enum') return 'enum';
  const p = t.presentation;
  return p === 'hex' || p === 'bin' || p === 'path' ? `${t.kind}, ${p}` : t.kind;
}
