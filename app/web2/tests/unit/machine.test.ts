import { describe, it, expect } from 'vitest';
import { readFileSync, readdirSync, statSync } from 'node:fs';
import { join } from 'node:path';
import { shortModel, formatRamKb } from '@/lib/machine';

// MMU presence (and every other per-model decision) must come from the C
// capability probe — `machine.profile(id).capabilities` — never from a
// regex on the model's display name. This lint guards against the old
// `/SE\/30|II/i` pattern (and its variants) creeping back into frontend
// logic.
describe('no model-name regex in frontend logic', () => {
  // vitest runs with cwd = the web2 package root.
  const srcDir = join(process.cwd(), 'src');
  // Matches the historical MMU-by-name regex and close variants.
  const banned = /\/\s*SE\\?\/?30\s*\|\s*II/i;

  function walk(dir: string): string[] {
    const out: string[] = [];
    for (const entry of readdirSync(dir)) {
      const p = join(dir, entry);
      if (statSync(p).isDirectory()) out.push(...walk(p));
      else if (/\.(ts|svelte)$/.test(entry)) out.push(p);
    }
    return out;
  }

  it('contains no /SE\\/30|II/ style model-name matching', () => {
    const offenders = walk(srcDir).filter((f) => banned.test(readFileSync(f, 'utf8')));
    expect(offenders).toEqual([]);
  });
});

describe('shortModel', () => {
  it.each([
    ['Macintosh Plus', 'Plus'],
    ['Macintosh SE/30', 'SE/30'],
    ['Macintosh IIci', 'IIci'],
    ['Quadra 700', 'Quadra 700'],
  ] as const)('%s -> %s', (input, expected) => {
    expect(shortModel(input)).toBe(expected);
  });
});

// RAM is a number (KB) everywhere but the label: the dialog used to
// send the label back, and three models' options did not parse as "N MB".
describe('formatRamKb', () => {
  it.each([
    [512, '512 KB'],
    [2560, '2.5 MB'],
    [4096, '4 MB'],
    [524288, '512 MB'],
  ] as const)('%d -> %s', (kb, label) => {
    expect(formatRamKb(kb)).toBe(label);
  });
});
