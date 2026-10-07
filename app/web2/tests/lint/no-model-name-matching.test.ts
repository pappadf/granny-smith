import { describe, it, expect } from 'vitest';
import { readFileSync, readdirSync, statSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join, relative } from 'node:path';

// Lint guard: the UI must derive machine capabilities
// from catalog.profile() (the `capabilities` / `video_slots` probe), NEVER by
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
    // "iix-iicx-se30-97221136.rom" (a filename) has no backslash, so it does not match this.
    why: 'regex literal matching the "SE/30" model name (use catalog.profile capabilities instead)',
    pattern: /SE\\\/30/,
  },
  {
    // Any regex `.test(...)` applied to a model identifier. Deliberately does
    // NOT include a bare `name` — filename/extension checks (ZIP_EXT.test(name))
    // are legitimate; this targets capability-by-model-name only. Name
    // *fragments* in a regex literal are caught by the rule above regardless
    // of the variable they are tested against.
    why: 'regex .test() on a model-name variable (derive capabilities from catalog.profile, not the name)',
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

describe('frontend never matches on the model name', () => {
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

// No hardware knowledge in the frontend at all: which buses, slots, cards and
// models a machine has, and what each can do, is the machine-description
// tree's to say (catalog.profile), so nothing under src/ may branch on a bus
// or slot kind, a card id or a model id -- the tree's ids are data the
// renderers pass back, never names they compare against.  (Literals that are
// not comparisons -- an icon named "floppy", a log category -- are fine.)
const MODEL_IDS = [
  'plus',
  'se30',
  'lisa',
  'macxl',
  'iix',
  'iicx',
  'iifx',
  'iici',
  'iisi',
  'q700',
  'q900',
  'q950',
  'q840av',
  'q660av',
  'pm6100',
  'pm7100',
  'pm8100',
  'pm7500',
  'pm8500',
  'pm9500',
  'ans500',
  'ans700',
  'pmg3dt',
  'pmg3mt',
];
const CARD_IDS = [
  'mdc_8_24',
  'display_card_24ac',
  '824gc',
  'builtin_se30_video',
  'builtin_rbv_video',
  'builtin_rbv_iisi_video',
  'tnt_control',
  'mach64_gx',
  'ati_rage_pro',
  'cirrus_54m30',
  'voodoo2',
  'voodoo2_webgpu',
  'rage128',
];
const BUS_AND_SLOT = ['nubus', 'pci', 'scsi\\d*', 'ata\\d*', 'profile', 'floppy', 'builtin'];
const ID = `(?:${[...MODEL_IDS, ...CARD_IDS, ...BUS_AND_SLOT].join('|')})`;
const ID_COMPARISON = new RegExp(
  `[!=]==?\\s*['"\`]${ID}['"\`]|['"\`]${ID}['"\`]\\s*[!=]==?|\\bcase\\s+['"\`]${ID}['"\`]`,
);

describe('the frontend never branches on a bus, slot, card or model id', () => {
  const files = walk(SRC);
  it('compares against none of them anywhere in src/', () => {
    const offenders: string[] = [];
    for (const file of files) {
      readFileSync(file, 'utf8')
        .split('\n')
        .forEach((line, i) => {
          if (ID_COMPARISON.test(line))
            offenders.push(`${relative(SRC, file)}:${i + 1}: ${line.trim()}`);
        });
    }
    expect(offenders, `hardware-id comparison found:\n${offenders.join('\n')}`).toEqual([]);
  });

  it('would catch one', () => {
    for (const line of [
      "if (bus.kind === 'profile') {",
      "x !== 'scsi2'",
      "case 'nubus':",
      'model == "iici"',
      "'mach64_gx' === card.id",
    ])
      expect(ID_COMPARISON.test(line), line).toBe(true);
    expect(ID_COMPARISON.test("icon: 'floppy'")).toBe(false);
  });
});

// Parameter memory is device state the core seeds from the configuration
// when it builds a machine; the frontend never writes it (no PRAM or NVRAM
// poke, no boot-device byte, on the boot path or anywhere else).
describe('the frontend never writes PRAM or NVRAM', () => {
  it('names no parameter-memory surface in src/', () => {
    const PRAM = /rtc\.pram|pram_init|\bnvram\b|boot_device/i;
    const offenders: string[] = [];
    for (const file of walk(SRC)) {
      readFileSync(file, 'utf8')
        .split('\n')
        .forEach((line, i) => {
          // Code, not prose: a comment may talk about PRAM.
          if (PRAM.test(line.replace(/\/\/.*$/, '')))
            offenders.push(`${relative(SRC, file)}:${i + 1}: ${line.trim()}`);
        });
    }
    expect(offenders, `parameter-memory access found:\n${offenders.join('\n')}`).toEqual([]);
  });
});
