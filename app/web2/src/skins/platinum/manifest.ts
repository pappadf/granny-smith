import type { SkinManifest } from '../types';

// Mac OS 8's "Platinum" appearance: grey bevelled chrome,
// folder tabs, white Finder lists with the lavender highlight, Platinum
// scrollbars, an inset well around the screen, LED status fields, Chivo
// (standing in for Charcoal) and two-tone Finder icons in its own sprite.
// It is built only from this folder and public/skins/platinum/.
export const platinum: SkinManifest = {
  id: 'platinum',
  name: 'Platinum',
  fonts: [
    {
      family: 'Chivo',
      src: 'skins/platinum/fonts/chivo-latin-wght.woff2',
      weight: '100 900',
      license: 'OFL-1.1',
    },
  ],
  overrides: () => import('./overrides.css'),
  metaThemeColor: '#dddddd',
};
