import type { SkinManifest } from '../types';

// Glass: floating translucent cards over a softly lit page.  Dark is
// "Midnight" (blue-black glass, periwinkle and teal), light is "Daylight"
// (white cards on cool grey, blue).  Sora and JetBrains Mono, the default
// sprite, and the floating layout in overrides.css.
export const glass: SkinManifest = {
  id: 'glass',
  name: 'Glass',
  schemes: ['dark', 'light'],
  fonts: [
    {
      family: 'Sora',
      src: 'skins/glass/fonts/sora-latin-wght.woff2',
      weight: '100 800',
      license: 'OFL-1.1',
    },
    {
      family: 'JetBrains Mono',
      src: 'skins/glass/fonts/jetbrains-mono-latin-wght.woff2',
      weight: '100 800',
      license: 'OFL-1.1',
    },
  ],
  overrides: () => import('./overrides.css'),
  metaThemeColor: { dark: '#090b11', light: '#eceef2' },
};
