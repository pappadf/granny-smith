// shell.usage for the console's signature hint and the command browser's
// details pane: the answers (kept per path until the machine changes), the
// declared argument's span in UTF-16, and a usage text as runs to render.

import { gsEval } from '@/bus/emulator';
import { highlightParts, loadHighlight, type HlSpan } from './highlight';
import { utf8ToUtf16 } from './utf8';

// The usage text, and for a method its signature (the text's first line)
// with each declared argument's [start, end) in it (UTF-8 bytes, as the
// core counts).
export interface UsageInfo {
  text: string;
  signature: string;
  argSpans: Array<[number, number] | null>;
}

const cache = new Map<string, Promise<UsageInfo | null>>();

// Forget every answer (the machine changed, and its methods with it).
export function forgetUsage(): void {
  cache.clear();
}

// `path`'s usage, or null.  No answer is not kept: the next ask tries again.
export function loadUsageInfo(path: string): Promise<UsageInfo | null> {
  if (!path) return Promise.resolve(null);
  const hit = cache.get(path);
  if (hit) return hit;
  const asked = readUsage(path);
  cache.set(path, asked);
  void asked.then((u) => {
    if (!u && cache.get(path) === asked) cache.delete(path);
  });
  return asked;
}

async function readUsage(path: string): Promise<UsageInfo | null> {
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

// Argument `index`'s [start, end) in the signature, in UTF-16 code units;
// null when there is no such argument (or no signature).
export function argSpan16(u: UsageInfo, index: number | null): [number, number] | null {
  const span = index !== null ? u.argSpans[index] : null;
  if (!span || !u.signature) return null;
  return [utf8ToUtf16(u.signature, span[0]), utf8ToUtf16(u.signature, span[1])];
}

// The code lines of a usage text -- the signature and the examples -- with
// where their code starts (after an example's lead).
const EXAMPLE_LEAD = /^(e\.g\. {2}| {6})/;

export function usageCodeLines(u: UsageInfo): Array<{ index: number; lead: number; text: string }> {
  const lines = u.text.split('\n');
  const out: Array<{ index: number; lead: number; text: string }> = [];
  if (u.signature && lines[0] === u.signature) out.push({ index: 0, lead: 0, text: lines[0] });
  let inExamples = false;
  for (let i = 1; i < lines.length; i++) {
    const m = EXAMPLE_LEAD.exec(lines[i]);
    if (m && (m[1].startsWith('e.g.') || inExamples)) {
      inExamples = true;
      out.push({ index: i, lead: m[1].length, text: lines[i].slice(m[1].length) });
    } else inExamples = false;
  }
  return out;
}

// The code lines' colours (shell.highlight), by line index.
export async function highlightUsage(u: UsageInfo): Promise<Record<number, HlSpan[]>> {
  const lines = usageCodeLines(u);
  const spans = await Promise.all(lines.map((l) => loadHighlight(l.text)));
  const out: Record<number, HlSpan[]> = {};
  lines.forEach((l, k) => {
    out[l.index] = spans[k].map((s) => ({ ...s, from: s.from + l.lead, to: s.to + l.lead }));
  });
  return out;
}

export interface UsageRun {
  text: string;
  cls: string | null;
  mark: boolean;
}

// The usage text as lines of runs: code lines coloured (`hl`, line index →
// spans), the marked argument cut out of the signature line.
export function usageRuns(
  u: UsageInfo,
  hl: Record<number, readonly HlSpan[]>,
  markArg: number | null,
): UsageRun[][] {
  const mark = u.text.startsWith(u.signature) ? argSpan16(u, markArg) : null;
  return u.text.split('\n').map((line, i) => {
    const runs: UsageRun[] = highlightParts(line, hl[i] ?? []).map((p) => ({
      text: p.text,
      cls: p.cls,
      mark: false,
    }));
    if (i !== 0 || !mark) return runs;
    const out: UsageRun[] = [];
    let at = 0;
    for (const r of runs) {
      const a = at;
      const b = at + r.text.length;
      at = b;
      const cuts = [a, Math.max(a, Math.min(b, mark[0])), Math.max(a, Math.min(b, mark[1])), b];
      for (let k = 0; k < 3; k++)
        if (cuts[k + 1] > cuts[k])
          out.push({ text: line.slice(cuts[k], cuts[k + 1]), cls: r.cls, mark: k === 1 });
    }
    return out;
  });
}
