import { describe, it, expect } from 'vitest';
import { modelHasMmu, shortModel, DEFAULT_CONFIG } from '@/lib/machine';

describe('modelHasMmu', () => {
  it.each([
    ['Macintosh SE/30', true],
    ['Macintosh IIci', true],
    ['Macintosh IIcx', true],
    ['Macintosh Plus', false],
    ['Macintosh SE', false],
  ] as const)('%s -> %s', (model, expected) => {
    expect(modelHasMmu(model)).toBe(expected);
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
