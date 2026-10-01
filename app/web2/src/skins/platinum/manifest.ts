import type { SkinManifest } from '../types';

// The proof skin: a Mac OS 8 "Platinum"-style look.  Light only, grey
// bevelled chrome, square corners, the lavender highlight, IBM Plex Sans
// (standing in for Charcoal), its own sprite and part-hook overrides.  It is
// built only from this folder and public/skins/platinum/.
export const platinum: SkinManifest = {
  id: 'platinum',
  name: 'Platinum',
  schemes: ['light'],
  sprite: 'skins/platinum/sprite.svg',
  fonts: [
    {
      family: 'IBM Plex Sans',
      src: 'skins/platinum/fonts/ibm-plex-sans-latin-wght.woff2',
      weight: '100 700',
      license: 'OFL-1.1',
    },
  ],
  overrides: () => import('./overrides.css'),
  metaThemeColor: { light: '#dddddd' },
};
