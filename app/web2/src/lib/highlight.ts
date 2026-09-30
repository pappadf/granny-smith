// Syntax highlighting from the core: shell.highlight(text) answers spans
// {start, end, class} in UTF-8 bytes; the console and the usage blocks
// work in UTF-16.  Every class is a palette colour (--gs-syntax-<class>).

import { gsEval } from '@/bus/emulator';
import { utf8ToUtf16 } from '@/lib/utf8';

export interface HlSpan {
  from: number; // UTF-16 offsets, half-open
  to: number;
  cls: string;
}

export interface HlPart {
  text: string;
  cls: string | null;
}

// The core's spans as UTF-16 offsets into `text`; malformed entries drop.
export function toUtf16Spans(text: string, raw: unknown): HlSpan[] {
  if (!Array.isArray(raw)) return [];
  const out: HlSpan[] = [];
  for (const r of raw) {
    if (!r || typeof r !== 'object') continue;
    const o = r as { start?: unknown; end?: unknown; class?: unknown };
    if (typeof o.start !== 'number' || typeof o.end !== 'number' || typeof o.class !== 'string')
      continue;
    const from = utf8ToUtf16(text, o.start);
    const to = utf8ToUtf16(text, o.end);
    if (to > from) out.push({ from, to, cls: o.class });
  }
  return out;
}

// shell.highlight for `text` (UTF-16 spans), [] when the core is not there.
export async function loadHighlight(text: string): Promise<HlSpan[]> {
  if (!text) return [];
  try {
    return toUtf16Spans(text, await gsEval('shell.highlight', [text]));
  } catch {
    return [];
  }
}

// `text` cut into runs, each with its class (null between spans).
export function highlightParts(text: string, spans: readonly HlSpan[]): HlPart[] {
  const out: HlPart[] = [];
  let at = 0;
  for (const s of spans) {
    if (s.from < at) continue; // overlapping: keep the first
    if (s.from > at) out.push({ text: text.slice(at, s.from), cls: null });
    out.push({ text: text.slice(s.from, s.to), cls: s.cls });
    at = s.to;
  }
  if (at < text.length) out.push({ text: text.slice(at), cls: null });
  return out;
}
