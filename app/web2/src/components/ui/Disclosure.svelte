<script lang="ts">
  import Icon from '@/components/common/Icon.svelte';
  import Spinner from './Spinner.svelte';

  // The open/closed arrow of a tree row or a section.  `hasChildren=false`
  // keeps the space and hides the glyph; `loading` shows a small spinner in
  // its place.  With `onclick` it is its own (pointer-only) button, as the
  // tree rows' arrows are; without, it is decorative.  `class` carries the
  // legacy hook (twistie); `open` and `has` are kept as classes for tests.
  interface Props {
    open: boolean;
    hasChildren?: boolean;
    loading?: boolean;
    onclick?: (ev: MouseEvent) => void;
    class?: string;
  }
  let { open, hasChildren = true, loading = false, onclick, class: cls = '' }: Props = $props();

  const phase = $derived(loading ? 'loading' : !hasChildren ? 'leaf' : open ? 'open' : 'closed');
</script>

{#if onclick}
  <!-- svelte-ignore a11y_click_events_have_key_events -->
  <span
    class="gs-disclosure {cls}"
    class:has={hasChildren}
    class:open
    data-state={phase}
    {onclick}
    role="button"
    tabindex="-1"
    aria-label={hasChildren ? (open ? 'Collapse' : 'Expand') : ''}
  >
    {#if loading}<Spinner size="sm" tone="current" />{:else if hasChildren}<Icon
        name="chevron"
        size="xs"
      />{/if}
  </span>
{:else}
  <span
    class="gs-disclosure {cls}"
    class:has={hasChildren}
    class:open
    data-state={phase}
    aria-hidden="true"
  >
    {#if loading}<Spinner size="sm" tone="current" />{:else if hasChildren}<Icon
        name="chevron"
        size="xs"
      />{/if}
  </span>
{/if}

<style>
  .gs-disclosure {
    display: inline-flex;
    align-items: center;
    justify-content: center;
    width: var(--gs-disclosure-size);
    flex-shrink: 0;
    color: var(--gs-disclosure-fg);
    /* The chevron points down when open and turns to point right when
       closed. */
    transform: rotate(var(--gs-disclosure-rotation-closed));
    transition: transform var(--gs-disclosure-duration) var(--gs-ease-out);
  }
  .gs-disclosure[data-state='open'],
  .gs-disclosure[data-state='loading'] {
    transform: none;
  }
  .gs-disclosure[data-state='leaf'] {
    visibility: hidden;
  }
</style>
