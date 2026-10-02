import { describe, it, expect, beforeEach } from 'vitest';
import {
  appearance,
  resolved,
  applyAppearance,
  applyUrlSkin,
  setSkin,
} from '@/state/appearance.svelte';
import { DEFAULT_SKIN } from '@/skins/registry';

beforeEach(() => {
  appearance.skin = DEFAULT_SKIN;
  appearance.sessionSkin = null;
  document.documentElement.removeAttribute('data-skin');
});

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

  it('Midnight is the default skin', () => {
    expect(DEFAULT_SKIN).toBe('midnight');
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
