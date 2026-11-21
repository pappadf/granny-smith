import { describe, it, expect } from 'vitest';
import { debugGlobalsEnabled, exposeDebug } from '@/lib/debugGlobal';

// A minimal window stand-in: the webdriver flag and the query string.
function fakeWindow(webdriver: boolean, search = ''): Window {
  return { navigator: { webdriver }, location: { search } } as unknown as Window;
}

describe('exposeDebug', () => {
  it('leaves nothing on an ordinary visit', () => {
    const w = fakeWindow(false);
    expect(exposeDebug('__x', 1, w)).toBe(false);
    expect((w as unknown as { __x?: number }).__x).toBeUndefined();
  });

  it('exposes under automation', () => {
    const w = fakeWindow(true);
    expect(exposeDebug('__x', 1, w)).toBe(true);
    expect((w as unknown as { __x?: number }).__x).toBe(1);
  });

  it('exposes with ?debug', () => {
    expect(debugGlobalsEnabled(fakeWindow(false, '?debug'))).toBe(true);
    expect(debugGlobalsEnabled(fakeWindow(false, '?rom=x&debug=1'))).toBe(true);
    expect(debugGlobalsEnabled(fakeWindow(false, '?rom=x'))).toBe(false);
  });
});
