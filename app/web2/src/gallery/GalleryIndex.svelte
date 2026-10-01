<script lang="ts">
  import { STORIES } from './registry';
  import { COVERAGE } from './coverage';
  import { TOKENS, type TokenSpec } from '@/styles/contract';
  import { skins, getSkin } from '@/skins/registry';
  import { appearance, applyAppearance, resolved } from '@/state/appearance.svelte';
  import type { Scheme } from '@/skins/types';

  // The gallery's index: a toolbar that switches the skin, the scheme and
  // reduced motion (for this page and every story it links to), and three
  // views: the stories, the token table (every contract token with its
  // resolved value here), and the coverage list (the UI inventory mapped to
  // the stories that show each element).
  const params = new URLSearchParams(window.location.search);
  type View = 'stories' | 'tokens' | 'coverage';
  let view = $state<View>(
    (['tokens', 'coverage'] as const).find((v) => v === params.get('view')) ?? 'stories',
  );
  let skin = $state(resolved.skin);
  let scheme = $state<Scheme>(resolved.scheme);
  let reduced = $state(params.get('motion') === 'reduced');
  let filter = $state('');

  const schemes = $derived(getSkin(skin).schemes);

  // Apply the toolbar to this page and keep the URL in step.
  $effect(() => {
    if (!schemes.includes(scheme)) scheme = schemes[0];
    appearance.sessionSkin = skin;
    appearance.schemeMode = scheme;
    applyAppearance();
    document.documentElement.classList.toggle('gs-gallery-reduced-motion', reduced);
    const q = query({
      skin,
      theme: scheme,
      motion: reduced ? 'reduced' : null,
      view: view === 'stories' ? null : view,
    });
    history.replaceState(null, '', `?gallery&${q}`);
  });

  // A query string from the set fields.
  function query(fields: Record<string, string | null>): string {
    return Object.entries(fields)
      .filter((e): e is [string, string] => e[1] !== null)
      .map(([k, v]) => `${k}=${encodeURIComponent(v)}`)
      .join('&');
  }

  // A story variant's page in the chosen look.
  function href(name: string, v: string): string {
    const q = query({
      story: name,
      variant: v,
      theme: scheme,
      skin,
      motion: reduced ? 'reduced' : null,
    });
    return `?gallery&${q}`;
  }

  // Every token's computed value on <html>, read again on each appearance
  // change.
  const values = $derived.by(() => {
    void resolved.version;
    const cs = getComputedStyle(document.documentElement);
    return new Map(TOKENS.map((t) => [t.name, cs.getPropertyValue(t.name).trim()]));
  });
  const shown = $derived(
    TOKENS.filter((t) => !filter || t.name.includes(filter) || t.doc.includes(filter)),
  );
  const swatch = (t: TokenSpec) => t.kind === 'color';

  const storyNames = new Set(STORIES.map((s) => s.name));
  // Coverage of one element: covered by a gallery story, only by the
  // app-states screenshots, by nothing on purpose, or missing.
  function coverage(stories: readonly string[]): 'story' | 'app' | 'none' | 'missing' {
    if (stories.some((s) => storyNames.has(s))) return 'story';
    if (stories.includes('app-states')) return 'app';
    if (stories.includes('none')) return 'none';
    return 'missing';
  }
  const missing = $derived(COVERAGE.filter((c) => coverage(c.stories) === 'missing').length);
</script>

<main class="gallery-index">
  <header class="bar">
    <h1>Granny Smith UI gallery</h1>
    <label
      >Skin <select bind:value={skin}>
        {#each skins as s (s.id)}<option value={s.id}>{s.name}</option>{/each}
      </select></label
    >
    <label
      >Scheme <select bind:value={scheme}>
        {#each schemes as s (s)}<option value={s}>{s}</option>{/each}
      </select></label
    >
    <label><input type="checkbox" bind:checked={reduced} /> Reduced motion</label>
    <nav>
      {#each ['stories', 'tokens', 'coverage'] as const as v (v)}
        <button type="button" aria-pressed={view === v} onclick={() => (view = v)}>{v}</button>
      {/each}
    </nav>
  </header>

  {#if view === 'stories'}
    <p>Every story and variant. Development builds only.</p>
    <table>
      <thead><tr><th>Story</th><th>Variants</th></tr></thead>
      <tbody>
        {#each STORIES as s (s.name)}
          <tr>
            <td>{s.name}</td>
            <td class="links">
              {#each s.variants as v (v)}<a href={href(s.name, v)}>{v}</a>{/each}
            </td>
          </tr>
        {/each}
      </tbody>
    </table>
  {:else if view === 'tokens'}
    <p>
      {TOKENS.length} tokens of the contract, resolved in {skin} / {scheme}.
      <input type="search" placeholder="Filter" bind:value={filter} aria-label="Filter tokens" />
    </p>
    <table class="tokens">
      <thead>
        <tr><th></th><th>Token</th><th>Layer</th><th>Kind</th><th>Value</th><th>Purpose</th></tr>
      </thead>
      <tbody>
        {#each shown as t (t.name)}
          <tr>
            <td
              >{#if swatch(t)}<span class="swatch" style:background="var({t.name})"></span>{/if}</td
            >
            <td><code>{t.name}</code></td>
            <td>{t.layer}</td>
            <td>{t.kind}</td>
            <td><code>{values.get(t.name)}</code></td>
            <td>{t.doc}</td>
          </tr>
        {/each}
      </tbody>
    </table>
  {:else}
    <p>
      The {COVERAGE.length} element types of the UI inventory and the stories that show them;
      {missing ? `${missing} without a story.` : 'every one is covered.'}
    </p>
    <table>
      <thead><tr><th>#</th><th>Element</th><th>Primitive</th><th>Shown in</th></tr></thead>
      <tbody>
        {#each COVERAGE as c (c.n)}
          {@const cov = coverage(c.stories)}
          <tr data-coverage={cov}>
            <td>{c.n}</td>
            <td>{c.element}</td>
            <td>{c.primitive}</td>
            <td class="links">
              {#each c.stories as s (s)}
                {#if storyNames.has(s)}<a
                    href={href(s, STORIES.find((x) => x.name === s)?.variants[0] ?? 'default')}
                    >{s}</a
                  >{:else if s === 'app-states'}<span>app-states screenshots</span
                  >{:else if s === 'none'}<span>not screenshot (native)</span>{:else}<span
                    class="missing">{s} (missing)</span
                  >{/if}
              {/each}
            </td>
          </tr>
        {/each}
      </tbody>
    </table>
  {/if}
</main>

<style>
  .gallery-index {
    padding: var(--gs-space-4) var(--gs-space-6);
    height: 100%;
    overflow: auto;
  }
  .bar {
    display: flex;
    align-items: center;
    flex-wrap: wrap;
    gap: var(--gs-space-4);
  }
  .bar h1 {
    font-size: var(--gs-font-size-xl);
    margin: 0;
  }
  nav {
    display: flex;
    gap: var(--gs-space-1);
  }
  nav button[aria-pressed='true'] {
    font-weight: var(--gs-font-weight-bold);
  }
  table {
    border-collapse: collapse;
  }
  td,
  th {
    padding: var(--gs-space-0-5) var(--gs-space-3) var(--gs-space-0-5) 0;
    text-align: left;
    vertical-align: top;
  }
  .links {
    display: flex;
    flex-wrap: wrap;
    gap: var(--gs-space-2);
  }
  a {
    color: var(--gs-text-link);
  }
  .swatch {
    display: inline-block;
    width: var(--gs-size-icon);
    height: var(--gs-size-icon);
    border: var(--gs-border-width) solid var(--gs-border);
  }
  tr[data-coverage='missing'] .missing {
    color: var(--gs-danger-fg);
  }
</style>
