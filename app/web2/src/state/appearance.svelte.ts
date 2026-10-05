import { untrack } from 'svelte';
import { DEFAULT_SKIN, getSkin } from '@/skins/registry';
import type { SkinManifest } from '@/skins/types';

// The appearance: which skin.  Every skin is a light or a dark one (its
// --gs-color-scheme token).  The user's choice is `appearance`; what is on
// screen is `resolved`.  applyAppearance() is the one writer of <html
// data-skin> and of the color-scheme / theme-color meta tags (index.html's
// pre-paint script sets data-skin once before the first frame; lint L-7).

interface AppearancePrefs {
  // The chosen skin (persisted as gs-skin).
  skin: string;
  // A ?skin= override for this page load only (never persisted).
  sessionSkin: string | null;
}

interface AppearanceResolved {
  skin: string;
  // Bumped after every DOM write, so code that reads token values knows to
  // read them again.
  version: number;
}

export const appearance: AppearancePrefs = $state({
  skin: DEFAULT_SKIN,
  sessionSkin: null,
});

export const resolved: AppearanceResolved = $state({
  skin: DEFAULT_SKIN,
  version: 0,
});

// The skin in effect: the session override, else the preference.
export function activeSkin(): SkinManifest {
  return getSkin(appearance.sessionSkin ?? appearance.skin);
}

export function setSkin(id: string): void {
  appearance.skin = getSkin(id).id;
  appearance.sessionSkin = null;
}

// Take a ?skin= parameter for this load (unknown ids are ignored).
export function applyUrlSkin(params: URLSearchParams): void {
  const id = params.get('skin');
  if (id && getSkin(id).id === id) appearance.sessionSkin = id;
}

// What has been loaded already: plain caches, never rendered.
// eslint-disable-next-line svelte/prefer-svelte-reactivity
const loadedFonts = new Set<string>();
// eslint-disable-next-line svelte/prefer-svelte-reactivity
const loadedOverrides = new Set<string>();
// Each skin's asset loads (fonts, overrides), settled or not.
// eslint-disable-next-line svelte/prefer-svelte-reactivity
const pending = new Map<string, Promise<unknown>[]>();

// Note a skin's asset load, so skinReady() can wait for it.
function track(id: string, p: Promise<unknown>): void {
  pending.set(id, [...(pending.get(id) ?? []), p.catch(() => undefined)]);
}

// Resolves once the active skin's fonts and overrides have loaded (or
// failed): the gallery waits for it before a screenshot.
export async function skinReady(): Promise<void> {
  await Promise.all(pending.get(activeSkin().id) ?? []);
}

// Register a skin's webfonts once (they swap in when loaded; the skin's
// --gs-font-* tokens list a system fallback after the family).
function loadFonts(skin: SkinManifest): void {
  if (typeof document === 'undefined' || typeof FontFace === 'undefined') return;
  for (const f of skin.fonts ?? []) {
    const key = `${f.family}|${f.src}|${f.weight ?? ''}|${f.style ?? ''}`;
    if (loadedFonts.has(key)) continue;
    loadedFonts.add(key);
    const face = new FontFace(f.family, `url(${f.src})`, {
      weight: f.weight,
      style: f.style,
    });
    document.fonts.add(face);
    track(skin.id, face.load());
  }
}

// Load a skin's overrides stylesheet once (its rules are scoped to its own
// data-skin, so it is harmless while another skin is active).
function loadOverrides(skin: SkinManifest): void {
  if (!skin.overrides || loadedOverrides.has(skin.id)) return;
  loadedOverrides.add(skin.id);
  track(
    skin.id,
    skin.overrides().catch(() => loadedOverrides.delete(skin.id)),
  );
}

// A <meta name=...> in <head>, created when missing.
function meta(name: string): HTMLMetaElement {
  let el = document.head.querySelector<HTMLMetaElement>(`meta[name="${name}"]`);
  if (!el) {
    el = document.createElement('meta');
    el.name = name;
    document.head.appendChild(el);
  }
  return el;
}

// Write the skin in effect to the document.
export function applyAppearance(): void {
  const skin = activeSkin();
  if (typeof document !== 'undefined') {
    const d = document.documentElement;
    if (d.dataset.skin !== skin.id) d.dataset.skin = skin.id;
    const css = getComputedStyle(d);
    meta('color-scheme').content = css.getPropertyValue('--gs-color-scheme').trim() || 'dark';
    const themeColor = skin.metaThemeColor ?? css.getPropertyValue('--gs-surface-raised').trim();
    if (themeColor) meta('theme-color').content = themeColor;
    loadFonts(skin);
    loadOverrides(skin);
    // data-skin-ready: the skin's fonts and overrides are in (automation
    // waits for it before a screenshot).
    delete d.dataset.skinReady;
    const id = skin.id;
    void skinReady().then(() => {
      if (d.dataset.skin === id) d.dataset.skinReady = '1';
    });
  }
  // Untracked: an effect running this must not depend on what it writes.
  untrack(() => {
    resolved.skin = skin.id;
    resolved.version++;
  });
}
