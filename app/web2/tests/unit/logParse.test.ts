import { describe, it, expect } from 'vitest';
import { parseLogLine } from '@/lib/logParse';

describe('parseLogLine', () => {
  it('parses a plain [cat] level msg line', () => {
    const e = parseLogLine('[cpu] 3 trap A-line\n');
    expect(e).not.toBeNull();
    expect(e!.cat).toBe('cpu');
    expect(e!.level).toBe(3);
    expect(e!.msg).toBe('trap A-line');
  });

  it('preserves trailing @ts prefix as part of msg', () => {
    const e = parseLogLine('[scsi] 1 @12345 reset\n');
    expect(e!.cat).toBe('scsi');
    expect(e!.level).toBe(1);
    expect(e!.msg).toBe('@12345 reset');
  });

  it('preserves PC= prefix as part of msg', () => {
    const e = parseLogLine('[mmu] 2 PC=0040A2C8 fault\n');
    expect(e!.msg).toBe('PC=0040A2C8 fault');
  });

  it('returns null for lines without the cat/level prefix', () => {
    expect(parseLogLine('just stdout text')).toBeNull();
    expect(parseLogLine('Booting Macintosh Plus...\n')).toBeNull();
    expect(parseLogLine('')).toBeNull();
  });

  it('rejects [cat] without a numeric level', () => {
    expect(parseLogLine('[cpu] hello')).toBeNull();
  });

  it('handles CRLF line endings', () => {
    const e = parseLogLine('[floppy] 0 ejected\r\n');
    expect(e!.cat).toBe('floppy');
    expect(e!.level).toBe(0);
    expect(e!.msg).toBe('ejected');
  });

  it('keeps spaces inside the body verbatim', () => {
    const e = parseLogLine('[via] 4   indented body  \n');
    expect(e!.msg).toBe('  indented body  ');
  });
});
