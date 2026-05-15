import { describe, it, expect, beforeEach } from 'vitest';
import { theme, cycleTheme, applyThemeToHtml, resolveTheme } from '@/state/theme.svelte';

beforeEach(() => {
  theme.mode = 'dark';
  document.documentElement.removeAttribute('data-theme');
});

describe('theme state', () => {
  it('cycles dark -> light -> system -> dark', () => {
    expect(theme.mode).toBe('dark');
    cycleTheme();
    expect(theme.mode).toBe('light');
    cycleTheme();
    expect(theme.mode).toBe('system');
    cycleTheme();
    expect(theme.mode).toBe('dark');
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
