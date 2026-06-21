import { describe, it, expect } from 'vitest';
import { readFileSync, readdirSync, statSync } from 'node:fs';
import { join } from 'node:path';
import { shortModel, DEFAULT_CONFIG } from '@/lib/machine';

// MMU presence (and every other per-model decision) must come from the C
// capability probe — `machine.profile(id).capabilities` — never from a
// regex on the model's display name. This lint guards against the old
// `/SE\/30|II/i` pattern (and its variants) creeping back into frontend
// logic; see proposal §1.4 / §6.1.
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

describe('DEFAULT_CONFIG', () => {
  it('has all six required fields', () => {
    expect(Object.keys(DEFAULT_CONFIG).sort()).toEqual(
      ['cd', 'floppies', 'hd', 'model', 'ram', 'vrom'].sort(),
    );
  });

  it('defaults to Macintosh Plus / 4 MB / hd1.img', () => {
    expect(DEFAULT_CONFIG.model).toBe('Macintosh Plus');
    expect(DEFAULT_CONFIG.ram).toBe('4 MB');
    expect(DEFAULT_CONFIG.hd).toBe('hd1.img');
  });
});
