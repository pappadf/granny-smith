import type { SkinManifest } from '../types';

// Starlight: Midnight's floating cards in daylight: white cards with soft
// shadows on a cool grey page, a blue accent, the toolbar as floating pills.
// Sora and JetBrains Mono (public/skins/glass/, shared with Midnight), the
// default sprite, and the glass layout plus its own touches as overrides.
export const starlight: SkinManifest = {
  id: 'starlight',
  name: 'Starlight',
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
  metaThemeColor: '#eceef2',
};
