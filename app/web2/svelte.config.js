import { vitePreprocess } from '@sveltejs/vite-plugin-svelte';

// Every component's <style> goes into the gs.components cascade layer
// (src/styles/layers.css), so a skin's overrides in gs.skin win over it
// without specificity tricks.  Svelte scopes selectors inside @layer as it
// does at the top level.  Lint L-10 checks this step stays configured.
const layerComponents = {
  name: 'gs-layer-components',
  style: ({ content }) => {
    if (!content.trim()) return;
    return { code: `@layer gs.components {\n${content}\n}` };
  },
};

export default {
  preprocess: [vitePreprocess(), layerComponents],
  compilerOptions: {
    runes: true,
  },
};
