import { untrack } from 'svelte';
import { DEFAULT_SKIN, getSkin } from '@/skins/registry';
import type { Scheme, SkinManifest } from '@/skins/types';

// The appearance: which skin, and which colour scheme (dark or light).  The
// user's preferences are `appearance`; what is on screen is `resolved`.
// applyAppearance() is the one writer of <html data-skin data-theme> and of
// the color-scheme / theme-color meta tags (index.html's pre-paint script
// sets them once before the first frame; lint L-7).

export type SchemeMode = Scheme | 'system';

interface AppearancePrefs {
  // The chosen skin (persisted as gs-skin).
  skin: string;
  // dark, light or system (persisted as gs-theme).
  schemeMode: SchemeMode;
  // A ?skin= override for this page load only (never persisted).
  sessionSkin: string | null;
}

interface AppearanceResolved {
  skin: string;
  scheme: Scheme;
  // Bumped after every DOM write, so code that reads token values knows to
  // read them again.
  version: number;
}

export const appearance: AppearancePrefs = $state({
  skin: DEFAULT_SKIN,
  schemeMode: 'system',
  sessionSkin: null,
});

export const resolved: AppearanceResolved = $state({
  skin: DEFAULT_SKIN,
  scheme: 'dark',
  version: 0,
});

// The OS colour preference, kept reactive so everything resolving `system`
// (the applied scheme, the toolbar's tooltip) follows an OS change.
const system = $state({ light: queryPrefersLight() });

// The OS colour preference right now.
function queryPrefersLight(): boolean {
  if (typeof window === 'undefined' || !window.matchMedia) return false;
  return window.matchMedia('(prefers-color-scheme: light)').matches;
}

// Whether the OS prefers light (tests set it through setSystemPrefersLight).
export function systemPrefersLight(): boolean {
  return system.light;
}

export function setSystemPrefersLight(light: boolean): void {
  system.light = light;
}

// The skin in effect: the session override, else the preference.
export function activeSkin(): SkinManifest {
  return getSkin(appearance.sessionSkin ?? appearance.skin);
}

// The scheme a preference resolves to on a skin: `system` follows the OS,
// and a scheme the skin lacks falls back to the skin's first.
export function resolveScheme(
  mode: SchemeMode,
  systemLight: boolean,
  skin: SkinManifest = activeSkin(),
): Scheme {
  const want: Scheme = mode === 'system' ? (systemLight ? 'light' : 'dark') : mode;
  return skin.schemes.includes(want) ? want : skin.schemes[0];
}

export function setSkin(id: string): void {
  appearance.skin = getSkin(id).id;
  appearance.sessionSkin = null;
}

// The appearance menu's name for one scheme of a skin.
export function lookName(skin: SkinManifest, scheme: Scheme): string {
  const named = skin.schemeNames?.[scheme];
  if (named) return named;
  if (skin.schemes.length < 2) return skin.name;
  return `${skin.name} ${scheme === 'dark' ? 'Dark' : 'Light'}`;
}

// Choose a look: a skin in one of its schemes.
export function setLook(id: string, scheme: Scheme): void {
  setSkin(id);
  appearance.schemeMode = scheme;
}

export function setSchemeMode(mode: SchemeMode): void {
  appearance.schemeMode = mode;
}

// Whether the active skin offers a choice of scheme.
export function canToggleScheme(): boolean {
  return activeSkin().schemes.length > 1;
}

// Flip to the opposite of the scheme on screen (the toolbar toggle), so a
// click from `system` lands on the visible opposite.  A single-scheme skin
// has nothing to flip to.
export function toggleScheme(): void {
  if (!canToggleScheme()) return;
  const now = resolveScheme(appearance.schemeMode, system.light);
  appearance.schemeMode = now === 'dark' ? 'light' : 'dark';
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

// Resolve the preferences and write them to the document.
export function applyAppearance(): void {
  const skin = activeSkin();
  const scheme = resolveScheme(appearance.schemeMode, system.light, skin);
  if (typeof document !== 'undefined') {
    const d = document.documentElement;
    if (d.dataset.skin !== skin.id) d.dataset.skin = skin.id;
    if (d.dataset.theme !== scheme) d.dataset.theme = scheme;
    meta('color-scheme').content = scheme;
    const themeColor =
      skin.metaThemeColor?.[scheme] ??
      getComputedStyle(d).getPropertyValue('--gs-surface-raised').trim();
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
    resolved.scheme = scheme;
    resolved.version++;
  });
}

// Follow OS preference changes (the one listener; App installs it).
// Returns the uninstaller.
export function installSystemSchemeListener(): () => void {
  if (typeof window === 'undefined' || !window.matchMedia) return () => undefined;
  const mq = window.matchMedia('(prefers-color-scheme: light)');
  const handler = () => {
    system.light = mq.matches;
  };
  mq.addEventListener('change', handler);
  return () => mq.removeEventListener('change', handler);
}
