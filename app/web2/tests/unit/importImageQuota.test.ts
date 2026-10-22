// S1018-42: an import that ran out of browser storage said only "could not
// store the image".  It names the cause when it had written about all that
// was free when it started.
import { describe, it, expect } from 'vitest';
import { storageFullHint } from '@/bus/importImage';

const MB = 1024 * 1024;

describe('storageFullHint', () => {
  it('names a full storage when the import wrote about all that was free', () => {
    expect(storageFullHint(1000 * MB, 968 * MB)).toBe(
      " (the browser's storage is full: it had 1000.0 MB free)",
    );
  });

  it('says nothing when the import stopped well short, or nothing is known', () => {
    expect(storageFullHint(1000 * MB, 496 * MB)).toBe('');
    expect(storageFullHint(null, 968 * MB)).toBe('');
  });
});
