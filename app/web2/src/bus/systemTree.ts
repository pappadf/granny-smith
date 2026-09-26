// Faithful projection of the C-side object model for the SYSTEM panel
// (proposal-system-object-model.md §8.2). There is NO allowlist: we render
// the root's children (the machine container + the emulator's meta service
// objects + the simulated network) and lazily expand each via
// meta.members (one call per node). Visibility is read off the model — the
// §7.2 three-tier category — so the tree cannot drift the way the old
// hand-maintained MACHINE_ROOTS list did.

import { gsEval, isModuleReady } from './emulator';
import type { IconName } from '@/lib/icons';

export interface SystemTreeNode {
  id: string; // full gsEval path (the dotted object path)
  label: string;
  icon?: IconName;
  desc?: string;
  leaf?: boolean;
  /** Divider group this row belongs to, for the readability headings
   *  (§8.2): 'machine' | 'emulator' | 'network'. Top-level only. */
  group?: 'machine' | 'emulator' | 'network';
}

// Top-level icon heuristic — purely cosmetic, keyed on the well-known
// object names. Unknown names render without an icon.
function iconFor(name: string): IconName | undefined {
  switch (name) {
    case 'machine':
      return 'computer';
    case 'cpu':
    case 'memory':
    case 'rom':
    case 'vrom':
    case 'via1':
    case 'via2':
    case 'scc':
    case 'rtc':
      return 'chip';
    case 'scsi':
      return 'hd';
    case 'floppy':
      return 'floppy';
    case 'sound':
      return 'speaker';
    case 'screen':
      return 'screen-full';
    case 'scheduler':
      return 'clock';
    case 'storage':
    case 'vfs':
      return 'folder';
    default:
      return undefined;
  }
}

// Classify a top-level object into one of the three §5.1 kinds.
function groupFor(name: string): 'machine' | 'emulator' | 'network' {
  if (name === 'machine') return 'machine';
  if (name === 'appletalk') return 'network';
  return 'emulator';
}

function asString(v: unknown, fallback: string): string {
  return typeof v === 'string' && v.length ? v : fallback;
}

// One member of a node, as meta.members describes it: every member in one
// round trip, where the tree used to spend two or three per member (F-46).
export interface MemberInfo {
  name: string;
  kind: 'attr' | 'child' | 'method';
  category: string; // basic | advanced | internal
  label: string;
  doc: string;
  readonly?: boolean; // attr
  value?: unknown; // attr, when values were asked for
  indexed?: boolean; // child
  indices?: number[]; // indexed child: its live entries
  // method: the method_info fields
  verb?: string;
  task?: string;
  destructive?: boolean;
  mutate?: boolean;
  hidden?: boolean;
  nargs?: number;
}

// `path`'s members ('' is the root), or [] when the call fails.
export async function loadMembers(path: string, values = false): Promise<MemberInfo[]> {
  const call = path ? `${path}.meta.members` : 'meta.members';
  const r = await gsEval(call, values ? [true] : []);
  if (!Array.isArray(r)) return [];
  return r.filter(
    (m): m is MemberInfo =>
      !!m && typeof m === 'object' && typeof (m as MemberInfo).name === 'string',
  );
}

// The root's children, faithfully — machine first, then the meta objects,
// then the network node. Each carries its model-owned label + group so the
// view can draw the §8.2 dividers without an allowlist.
export async function loadSystemRoots(): Promise<SystemTreeNode[]> {
  if (!isModuleReady()) return [];
  const out: SystemTreeNode[] = [];
  for (const m of await loadMembers('')) {
    if (m.kind !== 'child' || m.category === 'internal') continue; // never show internal nodes
    out.push({
      id: m.name,
      label: asString(m.label, m.name),
      icon: iconFor(m.name),
      group: groupFor(m.name),
    });
  }
  // machine first, network last, meta in the middle (stable within group).
  const rank = (g?: string) => (g === 'machine' ? 0 : g === 'network' ? 2 : 1);
  out.sort((a, b) => rank(a.group) - rank(b.group));
  return out;
}

// Children of a non-root node: attribute rows (leaf, with current value) then
// expandable child objects. Honours the §7.2 category — internal members are
// never shown; advanced members appear only when `showAdvanced` is on.
export async function loadSystemChildren(
  path: readonly string[],
  showAdvanced: boolean,
): Promise<SystemTreeNode[]> {
  if (!isModuleReady() || !path.length) return [];
  const target = path[path.length - 1];
  const out: SystemTreeNode[] = [];

  const visible = (cat: unknown): boolean => {
    if (cat === 'internal') return false;
    if (cat === 'advanced' && !showAdvanced) return false;
    return true;
  };

  // With values, in one call; a node whose values do not fit the reply (or
  // fail to read) is still shown, without them.
  let members = await loadMembers(target, true);
  if (!members.length) members = await loadMembers(target, false);

  // Attributes → leaf rows showing the live value.
  for (const m of members) {
    if (m.kind !== 'attr' || !visible(m.category)) continue;
    out.push({
      id: `${target}.${m.name}`,
      label: asString(m.label, m.name),
      desc: formatValue(m.value),
      leaf: true,
    });
  }

  // Child objects → expandable branches. An indexed-child member (a sparse
  // collection like scsi `device` / floppy `drive`) is expanded into its live
  // entries — `${target}[i]` — rather than shown as the bare collection member
  // (proposal §5.3). Attached children carry their own label and category.
  const seen = new Set(out.map((n) => n.label));
  for (const m of members) {
    if (m.kind !== 'child' || seen.has(m.name) || !visible(m.category)) continue;
    if (m.indexed) {
      // The bare integer routes to this member, so `${target}[i]` is the
      // canonical entry path.
      for (const i of m.indices ?? []) {
        if (typeof i !== 'number') continue;
        out.push({ id: `${target}[${i}]`, label: `[${i}]` });
      }
      continue;
    }
    out.push({ id: `${target}.${m.name}`, label: asString(m.label, m.name) });
  }

  return out;
}

// Method UI metadata as meta.method_info (and meta.members) return it.
export interface MethodInfo {
  name: string;
  verb: string;
  category: string;
  task: string;
  doc: string;
  destructive: boolean;
  mutate: boolean;
  hidden: boolean;
  nargs: number;
}

// The methods callable on a node, with their model-owned UI metadata —
// the source the SYSTEM tab's right-click menu and the command browser both
// render from (proposal §8.3 / §8.6). Hidden methods are filtered out.
export async function loadNodeMethods(path: string): Promise<MethodInfo[]> {
  if (!isModuleReady() || !path) return [];
  return methodsOf(await loadMembers(path));
}

// The visible methods among a node's members.
export function methodsOf(members: MemberInfo[]): MethodInfo[] {
  return members
    .filter((m) => m.kind === 'method' && !m.hidden)
    .map((m) => ({
      name: m.name,
      verb: m.verb ?? m.name,
      category: m.category,
      task: m.task ?? '',
      doc: m.doc,
      destructive: !!m.destructive,
      mutate: !!m.mutate,
      hidden: false,
      nargs: m.nargs ?? 0,
    }));
}

function formatValue(v: unknown): string {
  if (v === null || v === undefined) return '';
  if (typeof v === 'number') return String(v);
  if (typeof v === 'string') return v;
  if (typeof v === 'boolean') return v ? 'true' : 'false';
  try {
    return JSON.stringify(v);
  } catch {
    return String(v);
  }
}
