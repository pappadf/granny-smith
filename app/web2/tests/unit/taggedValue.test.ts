// Tagged values: one rule for what is an enum, object, error or container,
// and one text for each tag (the REPL's).
import { describe, it, expect } from 'vitest';
import {
  isContainer,
  isTagged,
  isTaggedEnum,
  isTaggedError,
  isTaggedObject,
  nestedText,
  tagText,
  valueShape,
} from '@/lib/taggedValue';
import { formatValue } from '@/lib/typeDescriptor';

const cpu = { object: 'cpu', name: 'cpu', path: 'machine.cpu' };

describe('taggedValue', () => {
  it('a tag has exactly its keys; any other object is a map', () => {
    expect(isTaggedEnum({ enum: 'turbo', index: 2 })).toBe(true);
    expect(isTaggedEnum({ enum: 'turbo', index: 2, x: 1 })).toBe(false);
    expect(isTaggedObject(cpu)).toBe(true);
    expect(isTaggedObject({ object: 'cpu', name: '' })).toBe(true);
    expect(isTaggedObject({ object: 'cpu', count: 3 })).toBe(false);
    expect(isTaggedError({ error: 'bad' })).toBe(true);
    expect(isTaggedError({ error: 'bad', code: 1 })).toBe(false);
    expect(isTagged([{ error: 'x' }])).toBe(false);
    expect(isTagged(null)).toBe(false);
  });

  it('prints tags as the REPL does', () => {
    expect(tagText({ enum: 'turbo', index: 2 })).toBe('turbo');
    expect(tagText({ enum: null, index: 7 })).toBe('<enum:7>');
    expect(tagText(cpu)).toBe('machine.cpu');
    expect(tagText({ object: 'cpu', name: 'main', path: '' })).toBe('<cpu:main>');
    expect(tagText({ error: 'bad' })).toBe('<error: bad>');
    expect(tagText({ a: 1 })).toBeNull();
    expect(tagText(3)).toBeNull();
  });

  it('containers are non-empty lists and untagged maps', () => {
    expect(isContainer([1])).toBe(true);
    expect(isContainer([])).toBe(false);
    expect(isContainer({ a: 1 })).toBe(true);
    expect(isContainer({})).toBe(false);
    expect(isContainer({ enum: 'x', index: 0 })).toBe(false);
    expect(isContainer(cpu)).toBe(false);
    // A map that merely has a tag's key is still a map.
    expect(isContainer({ object: 'x', count: 2 })).toBe(true);
    expect(isContainer('abc')).toBe(false);
  });

  it('shapes: object links, lists and maps by key, the rest scalar', () => {
    expect(valueShape(cpu)).toEqual({ kind: 'object', path: 'machine.cpu' });
    expect(valueShape({ object: 'cpu', name: 'x', path: '' })).toEqual({ kind: 'scalar' });
    expect(valueShape([5, 6])).toEqual({
      kind: 'list',
      items: [
        ['0', 5],
        ['1', 6],
      ],
    });
    expect(valueShape({ a: 1 })).toEqual({ kind: 'map', items: [['a', 1]] });
    expect(valueShape({ error: 'x' })).toEqual({ kind: 'scalar' });
    expect(valueShape([])).toEqual({ kind: 'scalar' });
  });

  it('nested text: strings quoted, tags as printed, containers by size', () => {
    expect(nestedText('a')).toBe('"a"');
    expect(nestedText(null)).toBe('null');
    expect(nestedText(4)).toBe('4');
    expect(nestedText([1, 2])).toBe('[2]');
    expect(nestedText({ a: 1, b: 2 })).toBe('{2}');
    expect(nestedText({ enum: null, index: 3 })).toBe('<enum:3>');
    // The same text as SYSTEM's formatValue.
    const pathless = { object: 'cpu', name: 'main', path: '' };
    expect(nestedText(pathless)).toBe(formatValue(pathless));
  });
});
