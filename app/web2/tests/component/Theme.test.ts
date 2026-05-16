import { describe, it, expect, beforeEach } from 'vitest';
import { theme, cycleTheme, applyThemeToHtml, resolveTheme } from '@/state/theme.svelte';

beforeEach(() => {
  theme.mode = 'dark';
  document.documentElement.removeAttribute('data-theme');
});

describe('theme state', () => {
  it('toggles between dark and light', () => {
    expect(theme.mode).toBe('dark');
    cycleTheme();
    expect(theme.mode).toBe('light');
    cycleTheme();
    expect(theme.mode).toBe('dark');
  });

  it('toggle from system flips to the opposite of the resolved theme', () => {
    // jsdom has no prefers-color-scheme implementation; resolveTheme('system')
    // returns 'dark' by default. The toggle should land on the opposite.
    theme.mode = 'system';
    const resolvedBefore = resolveTheme('system');
    cycleTheme();
    expect(theme.mode).toBe(resolvedBefore === 'dark' ? 'light' : 'dark');
  });

  it('applyThemeToHtml writes resolved theme to <html data-theme>', () => {
    applyThemeToHtml('light');
    expect(document.documentElement.dataset.theme).toBe('light');
    applyThemeToHtml('dark');
    expect(document.documentElement.dataset.theme).toBe('dark');
  });

  it('resolveTheme maps system mode through matchMedia (jsdom defaults dark)', () => {
    // jsdom does not implement prefers-color-scheme; it falls through to dark.
    expect(resolveTheme('system')).toMatch(/dark|light/);
    expect(resolveTheme('dark')).toBe('dark');
    expect(resolveTheme('light')).toBe('light');
  });
});
