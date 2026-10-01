import { describe, it, expect, beforeEach } from 'vitest';
import {
  appearance,
  resolved,
  applyAppearance,
  applyUrlSkin,
  resolveScheme,
  setSchemeMode,
  setSkin,
  setSystemPrefersLight,
  toggleScheme,
  canToggleScheme,
} from '@/state/appearance.svelte';
import { DEFAULT_SKIN } from '@/skins/registry';
import type { SkinManifest } from '@/skins/types';

beforeEach(() => {
  appearance.skin = DEFAULT_SKIN;
  appearance.schemeMode = 'dark';
  appearance.sessionSkin = null;
  setSystemPrefersLight(false);
  document.documentElement.removeAttribute('data-theme');
  document.documentElement.removeAttribute('data-skin');
});

// A skin with only a light scheme.
const lightOnly: SkinManifest = { id: 'paper', name: 'Paper', schemes: ['light'] };

describe('appearance', () => {
  it('toggles between dark and light', () => {
    toggleScheme();
    expect(appearance.schemeMode).toBe('light');
    toggleScheme();
    expect(appearance.schemeMode).toBe('dark');
  });

  it('a toggle from system lands on the opposite of what is shown', () => {
    appearance.schemeMode = 'system';
    setSystemPrefersLight(true);
    toggleScheme();
    expect(appearance.schemeMode).toBe('dark');
  });

  it('applyAppearance writes data-skin, data-theme and the meta tags', () => {
    setSchemeMode('light');
    applyAppearance();
    const d = document.documentElement;
    expect(d.dataset.skin).toBe(DEFAULT_SKIN);
    expect(d.dataset.theme).toBe('light');
    expect(document.querySelector('meta[name="color-scheme"]')?.getAttribute('content')).toBe(
      'light',
    );
    expect(resolved.scheme).toBe('light');
  });

  it('bumps resolved.version on every application', () => {
    const before = resolved.version;
    applyAppearance();
    applyAppearance();
    expect(resolved.version).toBe(before + 2);
  });

  it('system follows the OS preference', () => {
    appearance.schemeMode = 'system';
    setSystemPrefersLight(true);
    applyAppearance();
    expect(document.documentElement.dataset.theme).toBe('light');
    setSystemPrefersLight(false);
    applyAppearance();
    expect(document.documentElement.dataset.theme).toBe('dark');
  });

  it("a scheme the skin lacks falls back to the skin's first", () => {
    expect(resolveScheme('dark', false, lightOnly)).toBe('light');
    expect(resolveScheme('system', false, lightOnly)).toBe('light');
    expect(resolveScheme('light', false, lightOnly)).toBe('light');
  });

  it('the default skin has both schemes, so the toggle is offered', () => {
    expect(canToggleScheme()).toBe(true);
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
    applyUrlSkin(new URLSearchParams(`skin=${DEFAULT_SKIN}`));
    expect(appearance.sessionSkin).toBe(DEFAULT_SKIN);
  });
});
