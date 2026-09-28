// Command Browser catalogue for the Terminal view, now a projection of the one
// object model instead of a hand-maintained constant. The browser differs from
// the SYSTEM tab only in interaction (insert-text vs invoke) and grouping (by
// task vs by structure); both draw from the model, so neither can drift.
//
// Contents are generated at runtime from:
//   - object-node methods + global root verbs   (meta.members)
//   - user/builtin aliases                       (shell.aliases)
//   - shell-language keywords                    (static — they have no node)
// grouped by the model-owned task category, falling back to the owning
// top-level subsystem.

import { gsEval, isModuleReady } from '@/bus/emulator';
import { loadMembers, methodsOf, type MethodInfo } from '@/bus/systemTree';

export interface CommandNode {
  name: string;
  desc: string;
  insert?: string;
  children?: CommandNode[];
}

// Shell-language control-flow / binding keywords. These have
// no object node, so the command browser is their only browsable home. Kept
// in sync with object.c's RESERVED_WORDS (script-grammar subset).
const SHELL_KEYWORDS: { name: string; desc: string }[] = [
  { name: 'let', desc: 'Bind a shell variable:\n  let name = <expr>' },
  { name: 'if', desc: 'Conditional:\n  if <expr> { … } else { … }' },
  { name: 'else', desc: 'Else branch of an if.' },
  { name: 'while', desc: 'Loop while a predicate holds:\n  while <expr> { … }' },
  { name: 'for', desc: 'Iterate (reserved).' },
  { name: 'do', desc: 'Loop body (reserved).' },
  { name: 'return', desc: 'Return from a function (reserved).' },
  { name: 'break', desc: 'Break out of a loop (reserved).' },
  { name: 'continue', desc: 'Continue a loop (reserved).' },
];

const WALK_MAX_DEPTH = 4; // bounds the named-child descent

// The bucket a method falls into: its model-owned task category, else the
// owning top-level subsystem (first path segment), else "general".
function bucketFor(path: string, info: MethodInfo): string {
  if (info.task) return info.task;
  const seg = path.split('.')[0];
  return seg || 'general';
}

// Recursively collect methods under `path` via named children only (indexed
// collections are explored in the SYSTEM tab; here we keep the walk bounded).
async function walk(
  path: string,
  depth: number,
  into: Record<string, CommandNode[]>,
): Promise<void> {
  if (depth > WALK_MAX_DEPTH) return;
  // One call per node: its methods and its children (meta.members).
  const members = await loadMembers(path);
  // The root's own methods are the global verbs, gathered separately.
  if (path) {
    for (const info of methodsOf(members)) {
      const full = `${path}.${info.name}`;
      (into[bucketFor(path, info)] ??= []).push({ name: full, insert: full, desc: info.doc });
    }
  }
  for (const m of members) {
    if (m.kind !== 'child') continue;
    await walk(path ? `${path}.${m.name}` : m.name, depth + 1, into);
  }
}

// Build the command catalogue from the live model. Falls back to an empty
// tree when the module is not ready.
export async function buildCommandsTree(): Promise<CommandNode[]> {
  if (!isModuleReady()) return [];
  const groups: Record<string, CommandNode[]> = {};

  // Global root verbs (echo / download / help / …) — methods on the root.
  for (const info of methodsOf(await loadMembers(''))) {
    groups['general'] ??= [];
    groups['general'].push({ name: info.name, insert: info.name, desc: info.doc });
  }

  // Object-node methods, walked from the root's children.
  await walk('', 1, groups);

  const out: CommandNode[] = [];
  // Stable, readable group order: machine subsystems first-ish, then meta.
  for (const cat of Object.keys(groups).sort()) {
    const cmds = groups[cat].sort((a, b) => a.name.localeCompare(b.name));
    out.push({ name: cat, desc: `Commands grouped under "${cat}".`, children: cmds });
  }

  // Aliases — the $name shortcuts (shell.aliases → "name=path" strings).
  const aliases = await gsEval('shell.aliases');
  if (Array.isArray(aliases) && aliases.length) {
    out.push({
      name: 'Aliases',
      desc: 'Built-in and user $name shortcuts that expand to a path.',
      children: aliases
        .filter((a): a is string => typeof a === 'string')
        .map((a) => {
          const [name, path] = a.split('=');
          return { name, insert: `$${name}`, desc: `Alias for ${path ?? ''}` };
        }),
    });
  }

  // Shell-language keywords — no node, so this is their only home.
  out.push({
    name: 'Language',
    desc: 'Shell control-flow and binding keywords.',
    children: SHELL_KEYWORDS.map((k) => ({ name: k.name, insert: k.name, desc: k.desc })),
  });

  return out;
}
