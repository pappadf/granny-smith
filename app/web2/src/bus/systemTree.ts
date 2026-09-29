// meta.members, typed: one node's members (attributes with their values when
// asked, methods with their UI metadata and argument descriptors, children
// with their collection shape).  The SYSTEM tab (lib/systemRows) and the
// command browser (lib/commandsTree) both build from it.

import { gsEval } from './emulator';

// One member of a node, as meta.members describes it: every member in one
// round trip, where the tree used to spend two or three per member.
export interface MemberInfo {
  name: string;
  kind: 'attr' | 'child' | 'method';
  category: string; // basic | advanced | internal
  label: string;
  doc: string;
  readonly?: boolean; // attr
  value?: unknown; // attr, when values were asked for
  indexed?: boolean; // child
  indices?: number[] | null; // indexed child / collection container: its live entries
  keys?: string[] | null; // keyed collection: its live keys
  collection?: boolean; // child: a collection container (entries addressed [i] / ["k"])
  domain?: 'machine' | 'emulator' | 'network'; // root children
  type?: TypeDescriptor; // attr
  // The member's effective task (inherited down the tree), or null.
  task?: string | null;
  // method: the method_info fields
  verb?: string;
  destructive?: boolean;
  mutate?: boolean;
  hidden?: boolean;
  nargs?: number;
  args?: ArgInfo[];
  result?: TypeDescriptor;
  result_doc?: string;
  examples?: string[];
}

// What a value of a slot is (meta.members): kind, width, presentation, enum.
export interface TypeDescriptor {
  kind: string; // "uint", "enum", "string", …
  width: number;
  presentation: string | null; // "hex" | "path" | "bin" | "dec" | "sensitive" | null
  enum: string[] | null;
}

// One declared method argument.
export interface ArgInfo {
  name: string;
  doc: string;
  type: TypeDescriptor;
  optional: boolean;
  rest: boolean;
  default: unknown;
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
