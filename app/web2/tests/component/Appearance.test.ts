import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';
import {
  appearance,
  resolved,
  applyAppearance,
  applyUrlSkin,
  firstVisitSkin,
  setSkin,
} from '@/state/appearance.svelte';
import { DEFAULT_SKIN, LIGHT_DEFAULT_SKIN } from '@/skins/registry';

beforeEach(() => {
  appearance.skin = DEFAULT_SKIN;
  appearance.sessionSkin = null;
  document.documentElement.removeAttribute('data-skin');
});

afterEach(() => {
  vi.unstubAllGlobals();
});

// The operating system's colour preference, as matchMedia reports it.
function prefersLight(light: boolean) {
  vi.stubGlobal('matchMedia', (q: string) => ({
    matches: light && q === '(prefers-color-scheme: light)',
  }));
}

describe('appearance', () => {
  it('applyAppearance writes data-skin and the meta tags', () => {
    setSkin('platinum');
    applyAppearance();
    const d = document.documentElement;
    expect(d.dataset.skin).toBe('platinum');
    expect(document.querySelector('meta[name="theme-color"]')?.getAttribute('content')).toBe(
      '#dddddd',
    );
    expect(document.querySelector('meta[name="color-scheme"]')).not.toBeNull();
    expect(resolved.skin).toBe('platinum');
  });

  it('bumps resolved.version on every application', () => {
    const before = resolved.version;
    applyAppearance();
    applyAppearance();
    expect(resolved.version).toBe(before + 2);
  });

  it('a first visit gets Workbench, light or dark as the system prefers', () => {
    prefersLight(true);
    expect(firstVisitSkin()).toBe(LIGHT_DEFAULT_SKIN);
    prefersLight(false);
    expect(firstVisitSkin()).toBe(DEFAULT_SKIN);
  });

  it('setSkin takes only known ids and clears a session override', () => {
    appearance.sessionSkin = DEFAULT_SKIN;
    setSkin('no-such-skin');
    expect(appearance.skin).toBe(DEFAULT_SKIN);
    expect(appearance.sessionSkin).toBeNull();
  });

  it('?skin= sets a session skin only for known ids', () => {
    applyUrlSkin(new URLSearchParams('skin=no-such-skin'));
    expect(appearance.sessionSkin).toBeNull();
    applyUrlSkin(new URLSearchParams('skin=starlight'));
    expect(appearance.sessionSkin).toBe('starlight');
  });
});
