// Tagged values (VFMT_JSON_TAGGED): what a JSON value from the core is --
// an enum {enum, index}, an object {object, name, path}, an error {error},
// a list or map (a container), or a scalar -- and its text where the REPL
// prints it the same way.  A tag is a plain object with exactly the tag's
// keys; any other object is a map.

export interface TaggedEnum {
  enum: string | null;
  index: number;
}

export interface TaggedObject {
  object: string;
  name: string;
  path?: string;
}

export interface TaggedError {
  error: unknown;
}

function plainKeys(v: unknown): string[] | null {
  if (!v || typeof v !== 'object' || Array.isArray(v)) return null;
  return Object.keys(v);
}

export function isTaggedEnum(v: unknown): v is TaggedEnum {
  const k = plainKeys(v);
  return !!k && k.length === 2 && k.includes('enum') && k.includes('index');
}

export function isTaggedObject(v: unknown): v is TaggedObject {
  const k = plainKeys(v);
  return (
    !!k &&
    k.includes('object') &&
    k.includes('name') &&
    k.every((x) => x === 'object' || x === 'name' || x === 'path')
  );
}

export function isTaggedError(v: unknown): v is TaggedError {
  const k = plainKeys(v);
  return !!k && k.length === 1 && k[0] === 'error';
}

export function isTagged(v: unknown): v is TaggedEnum | TaggedObject | TaggedError {
  return isTaggedEnum(v) || isTaggedObject(v) || isTaggedError(v);
}

// A tagged value as the REPL prints it, or null for anything else: an enum
// by its label, an object by its path, an error with its message.
export function tagText(v: unknown): string | null {
  if (isTaggedEnum(v)) return v.enum ?? `<enum:${v.index}>`;
  if (isTaggedObject(v)) return v.path || `<${v.object}:${v.name}>`;
  if (isTaggedError(v)) return `<error: ${String(v.error)}>`;
  return null;
}

// A list or map with at least one item (something to expand).
export function isContainer(v: unknown): boolean {
  if (Array.isArray(v)) return v.length > 0;
  const k = plainKeys(v);
  return !!k && k.length > 0 && !isTagged(v);
}

// How a value renders in a tree: an object with a path (a link to its
// node), a non-empty list or map (expandable, items keyed by index or
// key), or a scalar (its text).
export type ValueShape =
  | { kind: 'object'; path: string }
  | { kind: 'list' | 'map'; items: [string, unknown][] }
  | { kind: 'scalar' };

export function valueShape(v: unknown): ValueShape {
  if (isTaggedObject(v) && v.path) return { kind: 'object', path: v.path };
  if (!isContainer(v)) return { kind: 'scalar' };
  if (Array.isArray(v)) return { kind: 'list', items: v.map((x, i) => [String(i), x]) };
  return { kind: 'map', items: Object.entries(v as Record<string, unknown>) };
}

// One nested value as text: strings quoted, tags as the REPL prints them,
// containers by size.
export function nestedText(v: unknown): string {
  if (v === null || v === undefined) return 'null';
  if (typeof v === 'string') return JSON.stringify(v);
  if (Array.isArray(v)) return `[${v.length}]`;
  const tag = tagText(v);
  if (tag !== null) return tag;
  if (typeof v === 'object') return `{${Object.keys(v).length}}`;
  return String(v);
}
