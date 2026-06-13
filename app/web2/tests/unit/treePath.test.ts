import { describe, it, expect } from 'vitest';
import {
  pathIsAncestorOrSelf,
  pathSiblingsOrSelf,
  parentPath,
  lastSegment,
  pathKey,
  pathKeyToArray,
  pathToLabel,
} from '@/lib/treePath';

describe('treePath', () => {
  describe('pathIsAncestorOrSelf', () => {
    it('returns true for identical paths', () => {
      expect(pathIsAncestorOrSelf(['a', 'b'], ['a', 'b'])).toBe(true);
    });
    it('returns true for strict descendants', () => {
      expect(pathIsAncestorOrSelf(['a'], ['a', 'b', 'c'])).toBe(true);
    });
    it('returns false for ancestor in the other direction', () => {
      expect(pathIsAncestorOrSelf(['a', 'b'], ['a'])).toBe(false);
    });
    it('returns false for unrelated paths', () => {
      expect(pathIsAncestorOrSelf(['a', 'b'], ['a', 'c'])).toBe(false);
    });
    it('empty ancestor matches everything', () => {
      expect(pathIsAncestorOrSelf([], ['a'])).toBe(true);
    });
  });

  describe('pathSiblingsOrSelf', () => {
    it('returns true for siblings under the same parent', () => {
      expect(pathSiblingsOrSelf(['a', 'b'], ['a', 'c'])).toBe(true);
    });
    it('returns false for paths of different depth', () => {
      expect(pathSiblingsOrSelf(['a'], ['a', 'b'])).toBe(false);
    });
    it('returns true for self', () => {
      expect(pathSiblingsOrSelf(['a', 'b'], ['a', 'b'])).toBe(true);
    });
  });

  describe('parentPath / lastSegment', () => {
    it('parent of a leaf is the dir', () => {
      expect(parentPath(['a', 'b', 'c'])).toEqual(['a', 'b']);
    });
    it('parent of root is empty', () => {
      expect(parentPath([])).toEqual([]);
    });
    it('lastSegment returns terminal id', () => {
      expect(lastSegment(['x', 'y'])).toBe('y');
      expect(lastSegment([])).toBe('');
    });
  });

  describe('pathKey / pathToLabel', () => {
    it('pathKey round-trips losslessly, even for segments with spaces', () => {
      const cases: string[][] = [
        ['/opfs', '/opfs/images'],
        // In-image names routinely contain spaces and punctuation; the key
        // must still round-trip to the exact same segments.
        ['/opfs', '/opfs/disk.img', '/opfs/disk.img/partition1/System Folder/Read Me'],
        [],
      ];
      for (const p of cases) {
        expect(pathKeyToArray(pathKey(p))).toEqual(p);
      }
    });
    it('distinct sibling paths with spaces produce distinct keys', () => {
      const a = pathKey(['/opfs', '/opfs/A B']);
      const b = pathKey(['/opfs', '/opfs/A', 'B']);
      expect(a).not.toBe(b);
    });
    it('pathToLabel returns terminal segment, or "/" for empty', () => {
      expect(pathToLabel([])).toBe('/');
      expect(pathToLabel(['/opfs', '/opfs/images/rom'])).toBe('/opfs/images/rom');
    });
  });
});
