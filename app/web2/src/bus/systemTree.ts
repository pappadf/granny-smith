// Faithful projection of the C-side object model for the SYSTEM panel
// (proposal-system-object-model.md §8.2). There is NO allowlist: we render
// the root's children (the machine container + the emulator's meta service
// objects + the simulated network) and lazily expand each via
// meta.children / meta.attributes. Visibility is read off the model — the
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

// The root's children, faithfully — machine first, then the meta objects,
// then the network node. Each carries its model-owned label + group so the
// view can draw the §8.2 dividers without an allowlist.
export async function loadSystemRoots(): Promise<SystemTreeNode[]> {
  if (!isModuleReady()) return [];
  const names = await gsEval('objects'); // root method: lists the root's children
  if (!Array.isArray(names)) return [];
  const out: SystemTreeNode[] = [];
  for (const name of names) {
    if (typeof name !== 'string') continue;
    const cat = await gsEval(`${name}.meta.category`);
    if (cat === 'internal') continue; // never show internal nodes
    const label = await gsEval(`${name}.meta.label`);
    out.push({ id: name, label: asString(label, name), icon: iconFor(name), group: groupFor(name) });
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
  showAdvanced: boolean
): Promise<SystemTreeNode[]> {
  if (!isModuleReady() || !path.length) return [];
  const target = path[path.length - 1];
  const out: SystemTreeNode[] = [];

  const visible = (cat: unknown): boolean => {
    if (cat === 'internal') return false;
    if (cat === 'advanced' && !showAdvanced) return false;
    return true;
  };

  // Attributes → leaf rows showing the live value.
  const attrs = await gsEval(`${target}.meta.attributes`);
  if (Array.isArray(attrs)) {
    for (const name of attrs) {
      if (typeof name !== 'string') continue;
      const cat = await gsEval(`${target}.meta.member_category`, [name]);
      if (!visible(cat)) continue;
      const label = await gsEval(`${target}.meta.member_label`, [name]);
      const v = await gsEval(`${target}.${name}`);
      out.push({ id: `${target}.${name}`, label: asString(label, name), desc: formatValue(v), leaf: true });
    }
  }

  // Child objects → expandable branches.
  const children = await gsEval(`${target}.meta.children`);
  if (Array.isArray(children)) {
    const seen = new Set(out.map((n) => n.label));
    for (const name of children) {
      if (typeof name !== 'string' || seen.has(name)) continue;
      const cat = await gsEval(`${target}.meta.member_category`, [name]);
      if (!visible(cat)) continue;
      const label = await gsEval(`${target}.meta.member_label`, [name]);
      out.push({ id: `${target}.${name}`, label: asString(label, name) });
    }
  }

  return out;
}

// Method UI metadata as returned by meta.method_info (JSON string).
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
  const names = await gsEval(`${path}.meta.methods`);
  if (!Array.isArray(names)) return [];
  const out: MethodInfo[] = [];
  for (const name of names) {
    if (typeof name !== 'string') continue;
    const raw = await gsEval(`${path}.meta.method_info`, [name]);
    if (typeof raw !== 'string') continue;
    try {
      const info = JSON.parse(raw) as MethodInfo;
      if (!info.hidden) out.push(info);
    } catch {
      // Tolerate a malformed descriptor — skip rather than break the menu.
    }
  }
  return out;
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
