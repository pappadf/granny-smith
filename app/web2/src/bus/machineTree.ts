// Walks the C-side object model to build the Machine panel's tree.
//
// We don't traverse the entire graph (that would be huge and would include
// internal-only subtrees). Instead we seed from a fixed allowlist of
// top-level paths that match the spec §4.3.1 example (CPU, Memory, VIA, SCC,
// IWM/SWIM, SCSI, Sound). Each root probes via gsEval('<path>.meta.children')
// and gsEval('<path>.meta.attributes'); roots that don't resolve are dropped
// silently — different models (Plus vs SE/30) expose different subtrees.
//
// Attributes are rendered as leaf rows with their current value; children
// recurse lazily when the user expands a branch.

import { gsEval, isModuleReady } from './emulator';
import type { IconName } from '@/lib/icons';

export interface MachineTreeNode {
  id: string;
  label: string;
  icon?: IconName;
  desc?: string;
  leaf?: boolean;
}

interface MachineRoot {
  path: string;
  label: string;
  icon: IconName;
}

const MACHINE_ROOTS: MachineRoot[] = [
  { path: 'cpu', label: 'CPU', icon: 'chip' },
  { path: 'memory', label: 'Memory', icon: 'chip' },
  { path: 'mmu', label: 'MMU', icon: 'chip' },
  { path: 'mac.via', label: 'VIA', icon: 'chip' },
  { path: 'mac.scc', label: 'SCC', icon: 'chip' },
  { path: 'floppy', label: 'Floppy (IWM)', icon: 'floppy' },
  { path: 'scsi', label: 'SCSI', icon: 'hd' },
  { path: 'sound', label: 'Sound', icon: 'speaker' },
  { path: 'scheduler', label: 'Scheduler', icon: 'chip' },
];

// Probe each root; keep only the ones whose meta call succeeds.
export async function loadMachineRoots(): Promise<MachineTreeNode[]> {
  if (!isModuleReady()) return [];
  const out: MachineTreeNode[] = [];
  for (const root of MACHINE_ROOTS) {
    const probe = await gsEval(`${root.path}.meta.children`);
    if (probe === null || probe === undefined) {
      // Try attributes instead — some leaf objects don't have children but
      // still respond to meta.attributes (e.g. machine.id).
      const attrs = await gsEval(`${root.path}.meta.attributes`);
      if (attrs === null || attrs === undefined) continue;
    }
    out.push({ id: root.path, label: root.label, icon: root.icon });
  }
  return out;
}

// Load children of a non-root node. `path` is an array of node ids where
// each id is the full gsEval path (e.g. ['cpu', 'cpu.pc']). We use the
// last segment as the dispatched path.
export async function loadMachineChildren(path: readonly string[]): Promise<MachineTreeNode[]> {
  if (!isModuleReady() || !path.length) return [];
  const target = path[path.length - 1];
  const out: MachineTreeNode[] = [];

  // Attributes first — they render as leaf rows with the current value.
  const attrs = await gsEval(`${target}.meta.attributes`);
  if (Array.isArray(attrs)) {
    for (const name of attrs) {
      if (typeof name !== 'string') continue;
      const fullPath = `${target}.${name}`;
      const v = await gsEval(fullPath);
      out.push({
        id: fullPath,
        label: name,
        desc: formatValue(v),
        leaf: true,
      });
    }
  }

  // Children — expandable branches. May overlap with attributes when the
  // C side exposes the same name in both lists; the attribute leaf wins
  // (already pushed first).
  const children = await gsEval(`${target}.meta.children`);
  if (Array.isArray(children)) {
    const seen = new Set(out.map((n) => n.label));
    for (const name of children) {
      if (typeof name !== 'string' || seen.has(name)) continue;
      out.push({ id: `${target}.${name}`, label: name });
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
