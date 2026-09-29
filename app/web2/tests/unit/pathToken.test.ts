// The path token under the console's cursor, and what a completion says
// about it.
import { describe, it, expect } from 'vitest';
import { pathTokenAt, replaceTokenAt, completionFocus, candidateName } from '@/lib/pathToken';

describe('pathToken', () => {
  it('finds the token around the cursor', () => {
    const line = 'echo machine.floppy.drive[0].ins 5';
    expect(pathTokenAt(line, 20)).toEqual({ start: 5, end: 32 });
    expect(pathTokenAt('a  b', 2)).toEqual({ start: 2, end: 2 });
  });

  it('replaces the token and puts the cursor after the insert', () => {
    expect(replaceTokenAt('mach', 4, 'machine.')).toEqual({ text: 'machine.', cursor: 8 });
    expect(replaceTokenAt('let x = mac + 1', 11, 'machine.cpu.pc')).toEqual({
      text: 'let x = machine.cpu.pc + 1',
      cursor: 22,
    });
    expect(replaceTokenAt('log.category["sc', 16, 'log.category["scsi"].')).toEqual({
      text: 'log.category["scsi"].',
      cursor: 21,
    });
    expect(replaceTokenAt('', 0, 'help ')).toEqual({ text: 'help ', cursor: 5 });
  });

  it('reads the resolved part and the partial segment of a token', () => {
    const line = 'machine.floppy.drive[0].ins';
    expect(completionFocus(line, { start: 24, end: 27 }, ['insert'])).toEqual({
      parent: 'machine.floppy.drive[0]',
      partial: 'ins',
      names: ['insert'],
      alias: null,
    });
    expect(
      completionFocus('machine.floppy.drive[', { start: 21, end: 21 }, ['0]', '1]']),
    ).toMatchObject({
      parent: 'machine.floppy.drive',
      names: ['0', '1'],
    });
    expect(completionFocus('log.category["sc', { start: 14, end: 16 }, ['scsi"]'])).toMatchObject({
      parent: 'log.category',
      partial: 'sc',
      names: ['scsi'],
    });
    // Whole-path candidates over the whole token (what the core answers).
    expect(completionFocus('files.l', { start: 0, end: 7 }, ['files.list', 'files.ls'])).toEqual({
      parent: 'files',
      partial: 'l',
      names: ['list', 'ls'],
      alias: null,
    });
    expect(
      completionFocus('x machine.floppy.drive[0].ins', { start: 2, end: 29 }, [
        'machine.floppy.drive[0].insert',
      ]),
    ).toMatchObject({ parent: 'machine.floppy.drive[0]', partial: 'ins', names: ['insert'] });
    expect(completionFocus('she', { start: 0, end: 3 }, ['shell.'])).toMatchObject({
      parent: '',
      names: ['shell'],
    });
    expect(completionFocus('echo $p', { start: 5, end: 7 }, ['$pc'])).toMatchObject({
      alias: 'p',
      names: ['pc'],
    });
  });

  it('candidate names lose the drill-in suffix', () => {
    expect(
      ['shell.', 'drive[', 'category["', 'path=', 'opfs/', 'insert'].map(candidateName),
    ).toEqual(['shell', 'drive', 'category', 'path', 'opfs', 'insert']);
  });
});
