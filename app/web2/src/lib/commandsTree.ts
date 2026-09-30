// The Terminal's command browser, as data: a STRUCTURAL view of the one
// object model -- levels are path segments, exactly as they are typed.
// Nothing here is hand-maintained: member rows come from meta.members, the
// commands from shell.command.list, the keywords from shell.keywords, the
// aliases from shell.alias.list, and a row's one-line description, type and
// usage text from the model (docs/internals/core/object/object-model.md).
// Only the sections that group them are fixed (SECTIONS).
//
// Member levels read through the shared structure cache (bus/memberStore);
// internal members are left out as a level is read.

import { gsEval, isModuleReady } from '@/bus/emulator';
import type { MemberInfo, TypeDescriptor } from '@/bus/systemTree';
import { DOMAIN_LABEL, collectionEntries, join, members } from '@/bus/memberStore';

export type RowKind =
  | 'section' // a top-level headline: Commands, a domain, Aliases, Language
  | 'object'
  | 'collection' // a collection container: its entries are addressed [i] / ["k"]
  | 'entry' // one entry of a collection
  | 'method'
  | 'attr'
  | 'group' // a synthetic grouping under a section (the alias groups)
  | 'alias'
  | 'keyword';

export interface BrowserRow {
  key: string; // unique across the tree (a model row's path, or a synthetic id)
  kind: RowKind;
  name: string; // what the row shows (`drive`, `[0]`, `["scsi"]`, `$pc`, `Commands`)
  word: string; // the name as a completion candidate spells it (`0`, `scsi`, `pc`); '' for a section
  path: string; // the full path ('' for synthetic rows)
  doc: string;
  insert: string; // what inserting the row writes over the path token at the cursor
  expandable: boolean;
  type?: TypeDescriptor; // attr
  readonly?: boolean; // attr
  keyed?: boolean; // collection: its entries are keyed by name
  // A synthetic row (section, group): where its rows come from.
  source?: () => Promise<BrowserRow[]>;
  defaultOpen?: boolean; // section: open until the user closes it
  rootMembers?: boolean; // section: its rows are root members (a path's first segment)
}

// The first sentence of a doc (up to the first ". " or the end).
export function firstSentence(doc: string): string {
  const i = doc.indexOf('. ');
  return i >= 0 ? doc.slice(0, i + 1) : doc;
}

// --- model rows -----------------------------------------------------------------

function shownMember(m: MemberInfo): boolean {
  return m.category !== 'internal' && !(m.kind === 'method' && m.hidden);
}

// The row a member of `path` becomes.
function memberRow(path: string, m: MemberInfo): BrowserRow {
  const full = join(path, m.name);
  const base = { key: full, name: m.name, word: m.name, path: full, doc: m.doc ?? '' };
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

// The rows directly under an object path: its attributes, methods and
// children in model order.
async function objectRows(path: string): Promise<BrowserRow[]> {
  return (await members(path)).filter(shownMember).map((m) => memberRow(path, m));
}

// The rows under a collection container: its live entries (by index, or by
// key), then the container's own members (add, clear, find, …) except the
// `entries` member itself.
async function collectionRows(path: string): Promise<BrowserRow[]> {
  // Re-read on every expansion: entries come and go.
  const ms = await members(path, true);
  const { entries, items } = await collectionEntries(path, ms);
  const out = items.map((e) => entryRow(e.name, e.word, e.path));
  for (const m of ms) if (m !== entries && shownMember(m)) out.push(memberRow(path, m));
  return out;
}

function entryRow(name: string, word: string, path: string): BrowserRow {
  return {
    key: path,
    kind: 'entry',
    name,
    word,
    path,
    doc: '',
    insert: `${path}.`,
    expandable: true,
  };
}

// The rows under any expandable row.
export async function expand(row: BrowserRow): Promise<BrowserRow[]> {
  if (row.source) return row.source();
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

// --- sections -------------------------------------------------------------------

// A top-level section: its headline, whether it starts open, and its rows.
export interface SectionProvider {
  key: string; // section:*
  label: string;
  doc: string;
  defaultOpen: boolean;
  rootMembers: boolean; // its rows are root members
  load(): Promise<BrowserRow[]>;
}

export const COMMANDS_KEY = 'section:commands';
export const ALIASES_KEY = 'section:aliases';
export const LANGUAGE_KEY = 'section:language';

const DOMAIN_DOC: Record<string, string> = {
  machine: 'The emulated computer',
  emulator: 'The emulator around it: running, files, checkpoints, debugging, logs',
  network: 'The simulated AppleTalk network',
};

// The sections, in order: the root's own verbs (Commands), each domain the
// root's children live in (in model order), then Aliases and Language.
function sections(root: MemberInfo[]): SectionProvider[] {
  // Root members as a section lists them (read again: the cache answers).
  const rootMembers = async () => (await members('')).filter(shownMember);
  const out: SectionProvider[] = [
    {
      key: COMMANDS_KEY,
      label: 'Commands',
      doc: "Words typed bare: the root's own methods and the commands",
      defaultOpen: true,
      rootMembers: true,
      load: async () => [
        ...(await rootMembers()).filter((m) => m.kind === 'method').map((m) => memberRow('', m)),
        ...(await loadCommands()).map(commandRow),
      ],
    },
  ];
  const domains: string[] = [];
  for (const m of root.filter(shownMember)) {
    const d = m.domain ?? 'emulator';
    if (m.kind === 'child' && !domains.includes(d)) domains.push(d);
  }
  for (const d of domains)
    out.push({
      key: `section:${d}`,
      label: DOMAIN_LABEL[d] ?? d,
      doc: DOMAIN_DOC[d] ?? '',
      defaultOpen: true,
      rootMembers: true,
      load: async () =>
        (await rootMembers())
          .filter((m) => m.kind === 'child' && (m.domain ?? 'emulator') === d)
          .map((m) => memberRow('', m)),
    });
  out.push(
    {
      key: ALIASES_KEY,
      label: 'Aliases',
      doc: 'Built-in and user $name shortcuts that expand to a path',
      defaultOpen: false,
      rootMembers: false,
      load: async () => ALIAS_GROUPS.map(groupRow),
    },
    {
      key: LANGUAGE_KEY,
      label: 'Language',
      doc: 'Shell keywords and their syntax',
      defaultOpen: false,
      rootMembers: false,
      load: keywordRows,
    },
  );
  return out;
}

function sectionRow(s: SectionProvider): BrowserRow {
  return {
    key: s.key,
    kind: 'section',
    name: s.label,
    word: '',
    path: '',
    doc: s.doc,
    insert: '',
    expandable: true,
    source: s.load,
    defaultOpen: s.defaultOpen,
    rootMembers: s.rootMembers,
  };
}

// The browser's root: one expandable headline per section.  A section's
// rows are listed at the headline's own indent.
export async function rootRows(): Promise<BrowserRow[]> {
  if (!isModuleReady()) return [];
  return sections(await members('')).map(sectionRow);
}

// --- commands -------------------------------------------------------------------

// A command: a bare word that runs a method (shell.command.list).
export interface ShellCommand {
  name: string;
  target: string;
  doc: string;
  builtin: boolean;
}

// The commands whose target is a method right now (`run` needs a machine).
export async function loadCommands(): Promise<ShellCommand[]> {
  const list = await gsEval('shell.command.list');
  if (!Array.isArray(list)) return [];
  return list
    .filter(
      (c): c is ShellCommand & { available?: boolean } =>
        !!c && typeof c === 'object' && typeof (c as ShellCommand).name === 'string',
    )
    .filter((c) => c.available !== false)
    .map((c) => ({
      name: c.name,
      target: typeof c.target === 'string' ? c.target : '',
      doc: typeof c.doc === 'string' ? c.doc : '',
      builtin: !!c.builtin,
    }));
}

// A command's row: typed bare, its usage that of the method it runs.
function commandRow(c: ShellCommand): BrowserRow {
  return {
    key: `cmd:${c.name}`,
    kind: 'method',
    name: c.name,
    word: c.name,
    path: c.target,
    doc: `${c.target}${c.doc ? ` — ${c.doc}` : ''}`,
    insert: `${c.name} `,
    expandable: false,
  };
}

// --- aliases and keywords -------------------------------------------------------

export interface AliasInfo {
  name: string;
  path: string;
  builtin: boolean;
}

// The Aliases groups: User, Built-in, and a Mac globals group for the
// built-ins over debug.mac.globals.
const ALIAS_GROUPS = [
  { key: 'group:aliases:user', label: 'User', doc: 'Aliases defined in this session' },
  { key: 'group:aliases:builtin', label: 'Built-in', doc: 'Aliases the shell defines' },
  {
    key: 'group:aliases:globals',
    label: 'Mac globals',
    doc: 'Built-in aliases for the Mac low-memory globals',
  },
];

// The Aliases group an alias is listed under.
export function aliasGroupKey(a: AliasInfo): string {
  if (!a.builtin) return 'group:aliases:user';
  return a.path.startsWith('debug.mac.globals.')
    ? 'group:aliases:globals'
    : 'group:aliases:builtin';
}

function groupRow(g: (typeof ALIAS_GROUPS)[number]): BrowserRow {
  return {
    key: g.key,
    kind: 'group',
    name: g.label,
    word: g.label,
    path: '',
    doc: g.doc,
    insert: '',
    expandable: true,
    source: async () =>
      (await loadAliases())
        .filter((a) => aliasGroupKey(a) === g.key)
        .map((a) => ({
          key: `alias:${a.name}`,
          kind: 'alias' as const,
          name: `$${a.name}`,
          word: a.name,
          path: a.path,
          doc: a.path,
          insert: `$${a.name}`,
          expandable: false,
        })),
  };
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

// Language: shell.keywords.
async function keywordRows(): Promise<BrowserRow[]> {
  const kws = await gsEval('shell.keywords');
  if (!Array.isArray(kws)) return [];
  return kws
    .filter((k): k is { word: string; syntax: string } => !!k && typeof k.word === 'string')
    .map((k) => ({
      key: `kw:${k.word}`,
      kind: 'keyword' as const,
      name: k.word,
      word: k.word,
      path: '',
      doc: k.syntax,
      insert: `${k.word} `,
      expandable: false,
    }));
}

// --- usage ----------------------------------------------------------------------

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
