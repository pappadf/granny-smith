<script lang="ts" generics="K extends string">
  import type { Snippet } from 'svelte';
  import { cycleListSelection, listKeyFromEvent } from '@/lib/keyboardNav';

  // One tab design in two variants: `panel` (the bottom panel's view tabs)
  // and `sub` (a strip inside a section, e.g. the MMU's State / Translate).
  // Selection is aria-selected; `tabClass` carries legacy hooks (ptab, tab).
  interface Props {
    tabs: ReadonlyArray<{ key: K; label: string; testid?: string }>;
    active: K;
    onSelect: (key: K) => void;
    variant?: 'panel' | 'sub';
    label?: string;
    tabClass?: string;
    class?: string;
    /** Optional right-edge accessory (e.g. the MMU section's S|U toggle). */
    accessory?: Snippet;
  }
  let {
    tabs,
    active,
    onSelect,
    variant = 'sub',
    label,
    tabClass = '',
    class: cls = '',
    accessory,
  }: Props = $props();

  // A sub strip is one tab stop (roving); the panel tabs stay all tabbable.
  const roving = $derived(variant === 'sub');

  function onKey(ev: KeyboardEvent) {
    const k = listKeyFromEvent(ev);
    if (!k) return;
    // Only the horizontal keys and Home/End: a focused tab inside a
    // scrolling container must not hijack ↑/↓.
    if (k === 'ArrowUp' || k === 'ArrowDown' || k === 'PageUp' || k === 'PageDown') return;
    const current = tabs.findIndex((t) => t.key === active);
    const next = cycleListSelection(tabs.length, current, k, { wrap: true });
    if (next >= 0 && next < tabs.length) {
      ev.preventDefault();
      onSelect(tabs[next].key);
    }
  }
</script>

<!-- svelte-ignore a11y_interactive_supports_focus -->
<div
  class="gs-tabs gs-tabs--{variant} {cls}"
  role="tablist"
  aria-label={label}
  onkeydown={roving ? onKey : undefined}
>
  {#each tabs as tab (tab.key)}
    <button
      type="button"
      class="gs-tabs__tab {tabClass}"
      data-tab={tab.key}
      data-testid={tab.testid}
      role="tab"
      aria-selected={tab.key === active}
      tabindex={roving ? (tab.key === active ? 0 : -1) : undefined}
      onclick={() => onSelect(tab.key)}
    >
      {tab.label}
    </button>
  {/each}
  {#if accessory}
    <span class="gs-tabs__accessory accessory">{@render accessory()}</span>
  {/if}
</div>

<style>
  .gs-tabs {
    display: flex;
  }
  /* Each variant maps its own component tokens onto the shared rules. */
  .gs-tabs--panel {
    --_height: var(--gs-tab-height);
    --_padding-x: var(--gs-tab-padding-x);
    --_line-height: var(--gs-tab-line-height);
    --_font-size: var(--gs-tab-font-size);
    --_font-weight: var(--gs-tab-font-weight);
    --_transform: var(--gs-tab-transform);
    --_tracking: var(--gs-tab-tracking);
    --_fg: var(--gs-tab-fg);
    --_fg-hover: var(--gs-tab-fg-hover);
    --_fg-selected: var(--gs-tab-fg-selected);
    --_indicator: var(--gs-tab-indicator-color);
    --_indicator-width: var(--gs-tab-indicator-width);
    --_indicator-inset: var(--gs-tab-indicator-inset);
    --_indicator-offset: var(--gs-tab-indicator-offset);
    --_reserve: 0px;
    flex: 1 1 auto;
    min-width: 0;
    /* Scroll horizontally with a hidden scrollbar (VS Code style). overflow-y
       must be hidden explicitly: auto on one axis forces visible->auto on the
       other, and on systems with classic (space-taking) scrollbars the
       horizontal bar shrinks the header below the tab height, cascading
       into both scrollbars appearing. */
    overflow-x: auto;
    overflow-y: hidden;
    scrollbar-width: none;
  }
  .gs-tabs--panel::-webkit-scrollbar {
    display: none;
  }
  .gs-tabs--panel > .gs-tabs__tab {
    align-self: center;
  }
  .gs-tabs--sub {
    --_height: var(--gs-tab-sub-height);
    --_padding-x: var(--gs-tab-sub-padding-x);
    --_line-height: var(--gs-tab-sub-line-height);
    --_font-size: var(--gs-tab-sub-font-size);
    --_font-weight: var(--gs-tab-sub-font-weight);
    --_transform: var(--gs-tab-sub-transform);
    --_tracking: var(--gs-tab-sub-tracking);
    --_fg: var(--gs-tab-sub-fg);
    --_fg-hover: var(--gs-tab-sub-fg-hover);
    --_fg-selected: var(--gs-tab-sub-fg-selected);
    --_indicator: var(--gs-tab-sub-indicator-color);
    --_indicator-width: var(--gs-tab-sub-indicator-width);
    --_indicator-inset: var(--gs-tab-sub-indicator-inset);
    --_indicator-offset: var(--gs-tab-sub-indicator-offset);
    /* The sub indicator sits under the label, so it reserves its width. */
    --_reserve: var(--gs-tab-sub-indicator-width);
    align-items: center;
    border-bottom: var(--gs-border-width) solid var(--gs-tab-sub-strip-border);
    background: var(--gs-tab-sub-strip-bg);
    height: var(--gs-tab-sub-height);
    flex-shrink: 0;
  }
  .gs-tabs__tab {
    position: relative;
    display: inline-flex;
    align-items: center;
    height: var(--_height);
    padding: 0 var(--_padding-x);
    background: transparent;
    border: none;
    border-bottom: var(--_reserve) solid transparent;
    color: var(--_fg);
    font-size: var(--_font-size);
    font-weight: var(--_font-weight);
    line-height: var(--_line-height);
    text-transform: var(--_transform);
    letter-spacing: var(--_tracking);
    cursor: pointer;
    white-space: nowrap;
  }
  .gs-tabs__tab:hover {
    color: var(--_fg-hover);
  }
  .gs-tabs__tab[aria-selected='true'] {
    color: var(--_fg-selected);
  }
  .gs-tabs__tab[aria-selected='true']::after {
    content: '';
    position: absolute;
    left: var(--_indicator-inset);
    right: var(--_indicator-inset);
    bottom: calc(var(--_indicator-offset) - var(--_reserve));
    height: var(--_indicator-width);
    background: var(--_indicator);
    pointer-events: none;
  }
  .gs-tabs__tab:focus-visible {
    outline: var(--gs-focus-width) solid var(--gs-focus-ring);
    outline-offset: -2px;
  }
  .gs-tabs__tab:disabled {
    opacity: var(--gs-opacity-disabled);
    cursor: default;
  }
  .gs-tabs__accessory {
    margin-left: auto;
    padding: 0 var(--gs-space-2);
    display: inline-flex;
    align-items: center;
  }
</style>
