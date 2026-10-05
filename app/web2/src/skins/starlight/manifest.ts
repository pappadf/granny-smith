import type { SkinManifest } from '../types';

// Starlight: Midnight in daylight, after the warm champagne of Apple's Starlight
// finish: pearl glass cards on an ivory page lit by gold and blush glows,
// a champagne-gold accent.  Sora and JetBrains Mono (public/skins/glass/,
// shared with Midnight), the default sprite, and the glass layout re-lit
// for a light page as overrides.
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
  metaThemeColor: '#f8f4ed',
};
