<script lang="ts" generics="K extends string">
  import type { Snippet } from 'svelte';
  import { cycleListSelection, listKeyFromEvent } from '@/lib/keyboardNav';
  import { openContextMenu } from '../common/ContextMenu.svelte';

  // One tab design in two variants: `panel` (the bottom panel's view tabs)
  // and `sub` (a strip inside a section, e.g. the MMU's State / Translate).
  // Selection is aria-selected; `tabClass` carries legacy hooks (ptab, tab).
  //
  // A panel strip never scrolls its tabs out of sight: the tabs that do not
  // fit move into a "»" menu at the end of the strip, and the active tab
  // always stays in the strip.  The widths come from an invisible copy of
  // the strip (same classes, so the skin's fonts and padding apply), and
  // `minWidth` reports the least the strip can show (the active tab and the
  // "»"), so a header can make room for it.
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
    /** Panel variant: the width the strip needs at least (bindable). */
    minWidth?: number;
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
    // Bindable prop, written by the effect below; the rule mistakes the
    // prop binding for a dead assignment.
    // eslint-disable-next-line no-useless-assignment
    minWidth = $bindable(0),
  }: Props = $props();

  const overflows = $derived(variant === 'panel');
  let stripEl = $state<HTMLDivElement | null>(null);
  let measureEl = $state<HTMLDivElement | null>(null);
  // What the strip has room for, and each tab's advance (its width plus the
  // gap or overlap before it) in the invisible copy.
  let avail = $state(Infinity);
  let advances = $state<number[]>([]);
  let moreAdvance = $state(0);
  let padEnd = $state(0);

  function measure() {
    if (!measureEl) return;
    const els = Array.from(measureEl.children) as HTMLElement[];
    const style = getComputedStyle(measureEl);
    const start = parseFloat(style.paddingLeft) || 0;
    let prev = start;
    const adv: number[] = [];
    for (const el of els) {
      const right = el.offsetLeft + el.offsetWidth;
      adv.push(right - prev);
      prev = right;
    }
    moreAdvance = adv.pop() ?? 0;
    advances = adv.map((a, i) => (i === 0 ? a + start : a));
    padEnd = parseFloat(style.paddingRight) || 0;
  }

  $effect(() => {
    if (!overflows || !stripEl || !measureEl || typeof ResizeObserver === 'undefined') return;
    const strip = stripEl;
    const ro = new ResizeObserver(() => {
      measure();
      avail = strip.clientWidth;
    });
    ro.observe(strip);
    ro.observe(measureEl);
    return () => ro.disconnect();
  });

  // Which tabs show: as many as fit in order, the active one always.
  const layout = $derived.by(() => {
    const all = tabs.map((_, i) => i);
    if (!overflows || advances.length !== tabs.length)
      return { shown: all, hidden: [] as number[] };
    const total = advances.reduce((a, b) => a + b, 0) + padEnd;
    if (total <= avail + 0.5) return { shown: all, hidden: [] as number[] };
    const budget = avail - moreAdvance - padEnd;
    const shown: number[] = [];
    let used = 0;
    for (const i of all) {
      if (used + advances[i] > budget) break;
      shown.push(i);
      used += advances[i];
    }
    const a = tabs.findIndex((t) => t.key === active);
    if (a >= 0 && !shown.includes(a)) {
      while (shown.length && used + advances[a] > budget) used -= advances[shown.pop()!];
      shown.push(a);
    }
    return { shown, hidden: all.filter((i) => !shown.includes(i)) };
  });

  $effect(() => {
    const a = tabs.findIndex((t) => t.key === active);
    minWidth = overflows && a >= 0 && advances.length ? advances[a] + moreAdvance + padEnd : 0;
  });

  function openMore(ev: MouseEvent) {
    const r = (ev.currentTarget as HTMLElement).getBoundingClientRect();
    openContextMenu(
      layout.hidden.map((i) => ({ label: tabs[i].label, action: () => onSelect(tabs[i].key) })),
      r.left,
      r.bottom,
    );
  }

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
  bind:this={stripEl}
  class="gs-tabs gs-tabs--{variant} {cls}"
  role="tablist"
  aria-label={label}
  onkeydown={roving ? onKey : undefined}
>
  {#each layout.shown.map((i) => tabs[i]) as tab (tab.key)}
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
  {#if layout.hidden.length}
    <button
      type="button"
      class="gs-tabs__tab gs-tabs__more"
      aria-haspopup="menu"
      aria-label="More views: {layout.hidden.map((i) => tabs[i].label).join(', ')}"
      title="More views"
      onclick={openMore}>»</button
    >
  {/if}
  {#if accessory}
    <span class="gs-tabs__accessory accessory">{@render accessory()}</span>
  {/if}
</div>
{#if overflows}
  <!-- The invisible copy every tab is measured in (and the "»"). -->
  <div
    bind:this={measureEl}
    class="gs-tabs gs-tabs--{variant} gs-tabs--measure {cls}"
    aria-hidden="true"
    inert
  >
    {#each tabs as tab (tab.key)}
      <!-- svelte-ignore a11y_role_supports_aria_props_implicit -->
      <button type="button" class="gs-tabs__tab" tabindex="-1" aria-selected={tab.key === active}
        >{tab.label}</button
      >
    {/each}
    <button type="button" class="gs-tabs__tab gs-tabs__more" tabindex="-1">»</button>
  </div>
{/if}

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
  /* Off-screen and invisible, at its natural width. */
  .gs-tabs.gs-tabs--measure {
    position: fixed;
    top: 0;
    left: -10000px;
    width: max-content;
    overflow: visible;
    visibility: hidden;
    pointer-events: none;
  }
  .gs-tabs__more {
    font-weight: var(--gs-font-weight-bold);
  }
  .gs-tabs__accessory {
    margin-left: auto;
    padding: 0 var(--gs-space-2);
    display: inline-flex;
    align-items: center;
  }
</style>
