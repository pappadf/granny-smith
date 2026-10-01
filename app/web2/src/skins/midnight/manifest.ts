import type { SkinManifest } from '../types';

// Midnight: floating translucent glass cards over a blue-black page lit by
// indigo and teal glows, a periwinkle accent.  Sora and JetBrains Mono
// (public/skins/glass/, shared with Starlight), the default sprite, and the
// glass layout (../glass.css) as its overrides, as drawn.
export const midnight: SkinManifest = {
  id: 'midnight',
  name: 'Midnight',
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
  overrides: () => import('../glass.css'),
  metaThemeColor: '#090b11',
};
