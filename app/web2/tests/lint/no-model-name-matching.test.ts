import { describe, it, expect } from 'vitest';
import { readFileSync, readdirSync, statSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join, relative } from 'node:path';

// Lint guard for proposal §1.4 / §6.1: the UI must derive machine capabilities
// from machine.profile() (the `capabilities` / `video_slots` probe), NEVER by
// matching on the human-readable model name. The original sin was
// `/SE\/30|II/i.test(model)` duplicated across machine.ts / upload.ts /
// emulator.ts / urlMedia.ts to decide MMU presence — so a new MMU machine
// whose name didn't match was silently classified as MMU-less.
//
// This test fails loudly if that class of pattern reappears anywhere under
// src/. It is intentionally a source-text scan (not an eslint rule) so it
// stays trivially auditable and needs no plugin.

const here = dirname(fileURLToPath(import.meta.url));
const SRC = join(here, '..', '..', 'src');

// Each rule: a description + a regex that should never match frontend source.
const FORBIDDEN: { why: string; pattern: RegExp }[] = [
  {
    // The smoking gun: an escaped-slash model name inside a regex literal.
    // "SE30.rom" (a filename) has no backslash, so it does not match this.
    why: 'regex literal matching the "SE/30" model name (use machine.profile capabilities instead)',
    pattern: /SE\\\/30/,
  },
  {
    // Any regex `.test(...)` applied to a model identifier. Deliberately does
    // NOT include a bare `name` — filename/extension checks (ZIP_EXT.test(name))
    // are legitimate; this targets capability-by-model-name only. Name
    // *fragments* in a regex literal are caught by the rule above regardless
    // of the variable they are tested against.
    why: 'regex .test() on a model-name variable (derive capabilities from machine.profile, not the name)',
    pattern: /\.test\(\s*(model|modelName|machineName|modelId)\b/,
  },
];

function walk(dir: string): string[] {
  const out: string[] = [];
  for (const entry of readdirSync(dir)) {
    const full = join(dir, entry);
    if (statSync(full).isDirectory()) {
      out.push(...walk(full));
    } else if (/\.(ts|svelte)$/.test(entry) && !/\.(test|spec)\./.test(entry)) {
      out.push(full);
    }
  }
  return out;
}

describe('frontend never matches on the model name (proposal §1.4)', () => {
  const files = walk(SRC);

  it('scans a non-trivial number of source files', () => {
    // Guard against a broken walk silently passing the test.
    expect(files.length).toBeGreaterThan(20);
  });

  for (const { why, pattern } of FORBIDDEN) {
    it(`has no occurrence of: ${why}`, () => {
      const offenders: string[] = [];
      for (const file of files) {
        const text = readFileSync(file, 'utf8');
        const lines = text.split('\n');
        lines.forEach((line, i) => {
          if (pattern.test(line)) offenders.push(`${relative(SRC, file)}:${i + 1}: ${line.trim()}`);
        });
      }
      expect(offenders, `model-name matching found:\n${offenders.join('\n')}`).toEqual([]);
    });
  }
});
