import { describe, it, expect } from 'vitest';
import {
  parseCheckpointDirName,
  formatCheckpointLabel,
  checkpointCreatedToDate,
  formatBytes,
} from '@/lib/checkpointMeta';

describe('parseCheckpointDirName', () => {
  it('parses a canonical dir name', () => {
    const r = parseCheckpointDirName('0123456789abcdef-20260516T143000Z');
    expect(r).not.toBeNull();
    expect(r!.id).toBe('0123456789abcdef');
    expect(r!.created).toBe('20260516T143000Z');
  });

  it('rejects names with non-hex id', () => {
    expect(parseCheckpointDirName('0123456789abcdeg-20260516T143000Z')).toBeNull();
  });

  it('rejects names with wrong-length id', () => {
    expect(parseCheckpointDirName('abc-20260516T143000Z')).toBeNull();
  });

  it('rejects names without the timestamp segment', () => {
    expect(parseCheckpointDirName('0123456789abcdef')).toBeNull();
  });
});

describe('formatCheckpointLabel', () => {
  it('formats a valid timestamp as "Checkpoint YYYY-MM-DD HH:MM"', () => {
    expect(formatCheckpointLabel('20260516T143000Z')).toBe('Checkpoint 2026-05-16 14:30');
  });

  it('falls back to raw input on malformed timestamp', () => {
    expect(formatCheckpointLabel('nope')).toBe('Checkpoint nope');
  });
});

describe('checkpointCreatedToDate', () => {
  it('round-trips to a Date in UTC', () => {
    const d = checkpointCreatedToDate('20260516T143000Z');
    expect(d).not.toBeNull();
    expect(d!.toISOString()).toBe('2026-05-16T14:30:00.000Z');
  });

  it('returns null on malformed input', () => {
    expect(checkpointCreatedToDate('bogus')).toBeNull();
  });
});

describe('formatBytes', () => {
  it('formats small numbers in bytes', () => {
    expect(formatBytes(512)).toBe('512 B');
  });
  it('scales up through KB / MB / GB', () => {
    expect(formatBytes(2 * 1024)).toBe('2.0 KB');
    expect(formatBytes(5 * 1024 * 1024)).toBe('5.0 MB');
    expect(formatBytes(2 * 1024 * 1024 * 1024)).toBe('2.0 GB');
  });
  it('zero and negative return "0 B"', () => {
    expect(formatBytes(0)).toBe('0 B');
    expect(formatBytes(-1)).toBe('0 B');
  });
});
