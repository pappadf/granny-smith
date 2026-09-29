// Values by their type descriptor (meta.members' {kind, width, presentation,
// enum}): the text SYSTEM shows for a value -- the same text the shell's REPL
// prints -- what an editor's text means (a literal of the slot's kind, or an
// expression the shell must evaluate), and the statement that repeats an
// edit or a call in the console.
//
// Values arrive tagged (VFMT_JSON_TAGGED): a hex uint as the string "0x1f",
// other integers as JSON numbers, an enum as {enum, index}, an object as
// {object, name, path}.

import type { TypeDescriptor } from '@/bus/systemTree';

// C's printf("%g"): six significant digits, trailing zeros dropped, an
// exponent outside 1e-4 .. 1e6.
export function formatG(x: number): string {
  if (!Number.isFinite(x)) return Number.isNaN(x) ? 'nan' : x > 0 ? 'inf' : '-inf';
  if (x === 0) return Object.is(x, -0) ? '-0' : '0';
  const exp = Math.floor(Math.log10(Math.abs(x)));
  const strip = (s: string) => (s.includes('.') ? s.replace(/\.?0+$/, '') : s);
  if (exp < -4 || exp >= 6) {
    const [m, e] = x.toExponential(5).split('e');
    const n = Number(e);
    return `${strip(m)}e${n < 0 ? '-' : '+'}${String(Math.abs(n)).padStart(2, '0')}`;
  }
  return strip(x.toFixed(Math.max(0, 5 - exp)));
}

function isTaggedEnum(v: unknown): v is { enum: string | null; index: number } {
  return !!v && typeof v === 'object' && 'enum' in v && 'index' in v;
}

function isTaggedObject(v: unknown): v is { object: string; name: string; path?: string } {
  return !!v && typeof v === 'object' && 'object' in v && 'name' in v;
}

// A value as the REPL prints it (lists and maps as compact JSON on one line).
export function formatValue(v: unknown, t?: TypeDescriptor): string {
  if (v === null || v === undefined) return '';
  if (t?.presentation === 'sensitive') return '••••';
  if (typeof v === 'boolean') return v ? 'true' : 'false';
  if (typeof v === 'string') return v; // strings as-is; a hex uint arrives as "0x…"
  if (typeof v === 'number') {
    if (t?.kind === 'float') return formatG(v);
    if (Number.isInteger(v) && (t?.presentation === 'hex' || t?.presentation === 'bin')) {
      const neg = v < 0;
      // The REPL prints a negative hex int as its two's complement.
      const u = neg ? BigInt.asUintN(64, BigInt(v)) : BigInt(v);
      return t.presentation === 'hex' ? `0x${u.toString(16)}` : `0b${u.toString(2)}`;
    }
    return Number.isInteger(v) ? String(v) : formatG(v);
  }
  if (isTaggedEnum(v)) return v.enum ?? `<enum:${v.index}>`;
  if (isTaggedObject(v)) return v.path || `<${v.object}:${v.name}>`;
  if (v && typeof v === 'object' && 'error' in v)
    return `<error: ${String((v as { error: unknown }).error)}>`;
  try {
    return JSON.stringify(v);
  } catch {
    return String(v);
  }
}

// --- Parsing an editor's text ------------------------------------------------

// What committing a text does: write a literal with gsEval, or run the
// statement `<path> = <text>` in the console (an expression, or an integer
// too big for a JSON number).
export type Commit = { mode: 'literal'; value: unknown } | { mode: 'statement' };

const INT_RE = /^([+-])?(0x[0-9a-f]+|0b[01]+|[0-9]+)$/i;
const FLOAT_RE = /^[+-]?(\d+\.?\d*|\.\d+)(e[+-]?\d+)?$/i;

// An integer literal's value, or null when the text is not one.
export function parseInteger(text: string): bigint | null {
  const m = INT_RE.exec(text.trim());
  if (!m) return null;
  const body = m[2].toLowerCase();
  const v = BigInt(body); // BigInt reads 0x / 0b / decimal
  return m[1] === '-' ? -v : v;
}

// How to commit `text` to a slot of type `t`.
export function parseCommit(text: string, t?: TypeDescriptor): Commit {
  const kind = t?.kind ?? 'any';
  const s = text.trim();
  switch (kind) {
    case 'int':
    case 'uint': {
      const v = parseInteger(s);
      if (v === null) return { mode: 'statement' };
      const big = v > BigInt(Number.MAX_SAFE_INTEGER) || v < -BigInt(Number.MAX_SAFE_INTEGER);
      return big ? { mode: 'statement' } : { mode: 'literal', value: Number(v) };
    }
    case 'float':
      if (FLOAT_RE.test(s)) return { mode: 'literal', value: Number(s) };
      return parseInteger(s) !== null
        ? { mode: 'literal', value: Number(parseInteger(s)) }
        : { mode: 'statement' };
    case 'bool':
      if (s === 'true' || s === 'false') return { mode: 'literal', value: s === 'true' };
      return { mode: 'statement' };
    case 'enum':
      if (t?.enum?.includes(s)) return { mode: 'literal', value: s };
      return { mode: 'statement' };
    case 'string':
    case 'bytes':
      // For a string, any text is the literal.
      return { mode: 'literal', value: text };
    default:
      return { mode: 'statement' };
  }
}

// --- Statements --------------------------------------------------------------

// A string as a shell literal: double-quoted, `\` and `"` escaped.
export function quoteString(s: string): string {
  return `"${s.replace(/\\/g, '\\\\').replace(/"/g, '\\"')}"`;
}

// A value as it would be typed: REPL text for numbers (hex for a hex slot),
// strings quoted.
export function valueText(v: unknown, t?: TypeDescriptor): string {
  if (typeof v === 'string') {
    if (t?.kind === 'enum') return quoteString(v);
    if (t?.kind === 'int' || t?.kind === 'uint') return v; // already REPL text ("0x1f")
    return quoteString(v);
  }
  if (typeof v === 'number' && t?.presentation === 'hex' && Number.isInteger(v))
    return formatValue(v, t);
  if (isTaggedEnum(v)) return quoteString(v.enum ?? String(v.index));
  return formatValue(v, t);
}

// `path = value`, as the console would run it.
export function assignStatement(path: string, v: unknown, t?: TypeDescriptor): string {
  return `${path} = ${valueText(v, t)}`;
}

// `path.method arg …` in argument mode (no arguments: the bare path).
export function callStatement(
  path: string,
  args: readonly unknown[],
  types: readonly (TypeDescriptor | undefined)[] = [],
): string {
  const parts = args.map((a, i) => valueText(a, types[i]));
  return parts.length ? `${path} ${parts.join(' ')}` : path;
}
