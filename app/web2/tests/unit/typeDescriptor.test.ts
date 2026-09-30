// Values by type descriptor: REPL-identical text, what an edit commits, and
// the statements SYSTEM echoes.
import { describe, it, expect } from 'vitest';
import {
  formatG,
  formatValue,
  parseCommit,
  parseInteger,
  quoteString,
  assignStatement,
  callStatement,
} from '@/lib/typeDescriptor';
import type { TypeDescriptor } from '@/bus/systemTree';

const t = (kind: string, presentation: string | null = null, e: string[] | null = null) =>
  ({ kind, width: 4, presentation, enum: e }) as TypeDescriptor;

describe('formatValue (as the REPL prints)', () => {
  it('a hex uint arrives as its text', () => {
    expect(formatValue('0x408986', t('uint', 'hex'))).toBe('0x408986');
  });
  it('an int is a decimal number; a hex int prints in hex', () => {
    expect(formatValue(42, t('int'))).toBe('42');
    expect(formatValue(-3, t('int'))).toBe('-3');
    expect(formatValue(255, t('int', 'hex'))).toBe('0xff');
    expect(formatValue(-1, t('int', 'hex'))).toBe('0xffffffffffffffff');
    expect(formatValue(5, t('uint', 'bin'))).toBe('0b101');
  });
  it('an enum by name, a bool as a word, an object by path', () => {
    expect(formatValue({ enum: 'turbo', index: 2 }, t('enum'))).toBe('turbo');
    expect(formatValue({ enum: null, index: 7 }, t('enum'))).toBe('<enum:7>');
    expect(formatValue(true, t('bool'))).toBe('true');
    expect(formatValue({ object: 'cpu', name: 'cpu', path: 'machine.cpu' })).toBe('machine.cpu');
  });
  it('a float as %g, a sensitive value hidden', () => {
    expect(formatValue(1.5, t('float'))).toBe('1.5');
    expect(formatValue(1234567, t('float'))).toBe('1.23457e+06');
    expect(formatValue('hunter2', t('string', 'sensitive'))).toBe('••••');
  });
  it('lists and maps as compact JSON', () => {
    expect(formatValue([1, 2])).toBe('[1,2]');
    expect(formatValue({ a: 1 })).toBe('{"a":1}');
  });
});

describe('formatG', () => {
  it.each([
    [0, '0'],
    [100, '100'],
    [0.1, '0.1'],
    [1 / 3, '0.333333'],
    [123456, '123456'],
    [1e6, '1e+06'],
    [0.0001, '0.0001'],
    [0.00001, '1e-05'],
    [-2.5e-7, '-2.5e-07'],
  ])('%s → %s', (x, s) => expect(formatG(x)).toBe(s));
});

describe('parseCommit', () => {
  it('integer literals in hex, decimal and binary are written as numbers', () => {
    expect(parseCommit('0x1234', t('uint', 'hex'))).toEqual({ mode: 'literal', value: 0x1234 });
    expect(parseCommit(' 42 ', t('int'))).toEqual({ mode: 'literal', value: 42 });
    expect(parseCommit('0b101', t('uint'))).toEqual({ mode: 'literal', value: 5 });
    expect(parseCommit('-7', t('int'))).toEqual({ mode: 'literal', value: -7 });
  });
  it('an integer above 2^53 takes the statement path', () => {
    expect(parseCommit('0xffffffffffffffff', t('uint', 'hex'))).toEqual({ mode: 'statement' });
    expect(parseInteger('0xffffffffffffffff')).toBe(0xffffffffffffffffn);
  });
  it('an expression takes the statement path', () => {
    expect(parseCommit('$a5 + 0x10', t('uint', 'hex'))).toEqual({ mode: 'statement' });
  });
  it('bools, enum names, floats and strings', () => {
    expect(parseCommit('true', t('bool'))).toEqual({ mode: 'literal', value: true });
    expect(parseCommit('yes', t('bool'))).toEqual({ mode: 'statement' });
    expect(parseCommit('turbo', t('enum', null, ['paced', 'turbo']))).toEqual({
      mode: 'literal',
      value: 'turbo',
    });
    expect(parseCommit('max', t('enum', null, ['paced', 'turbo']))).toEqual({ mode: 'statement' });
    expect(parseCommit('2.5e3', t('float'))).toEqual({ mode: 'literal', value: 2500 });
    expect(parseCommit('any "text"', t('string'))).toEqual({
      mode: 'literal',
      value: 'any "text"',
    });
  });
});

describe('statements', () => {
  it('assignments in REPL text; strings quoted with escapes', () => {
    expect(assignStatement('machine.cpu.d0', 0x1234, t('uint', 'hex'))).toBe(
      'machine.cpu.d0 = 0x1234',
    );
    expect(assignStatement('log.category["scsi"].level', 5, t('int'))).toBe(
      'log.category["scsi"].level = 5',
    );
    expect(assignStatement('x.name', 'a "b" \\c', t('string'))).toBe('x.name = "a \\"b\\" \\\\c"');
    expect(assignStatement('scheduler.mode', 'turbo', t('enum'))).toBe('scheduler.mode = "turbo"');
    expect(assignStatement('x.on', false, t('bool'))).toBe('x.on = false');
  });
  it('calls in argument mode', () => {
    expect(callStatement('machine.scsi.device[3].eject', [])).toBe('machine.scsi.device[3].eject');
    expect(
      callStatement('debug.breakpoints.add', [0x408000, 'x y'], [t('uint', 'hex'), t('string')]),
    ).toBe('debug.breakpoints.add 0x408000 "x y"');
    expect(quoteString('')).toBe('""');
  });
});
