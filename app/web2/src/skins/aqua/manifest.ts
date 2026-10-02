import type { SkinManifest } from '../types';

// Mac OS X 10.0's "Aqua": pinstripes, a brushed toolbar, blue
// and white gel capsules, tabs as one capsule over a recessed box, the blue
// gradient selection, gel scrollbars and a white bezel around the screen.
// Hanken Grotesk stands in for Lucida Grande, Fira Mono for Monaco.
export const aqua: SkinManifest = {
  id: 'aqua',
  name: 'Aqua',
  fonts: [
    {
      family: 'Hanken Grotesk',
      src: 'skins/aqua/fonts/hanken-grotesk-latin-wght.woff2',
      weight: '100 900',
      license: 'OFL-1.1',
    },
    {
      family: 'Fira Mono',
      src: 'skins/aqua/fonts/fira-mono-latin-400.woff2',
      weight: '400',
      license: 'OFL-1.1',
    },
  ],
  overrides: () => import('./overrides.css'),
  metaThemeColor: '#e7e7e7',
};
