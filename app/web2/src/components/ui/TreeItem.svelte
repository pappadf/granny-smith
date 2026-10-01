<script lang="ts">
  import type { Snippet } from 'svelte';
  import type { HTMLAttributes } from 'svelte/elements';
  import Icon from '@/components/common/Icon.svelte';
  import type { IconName } from '@/lib/icons';
  import Disclosure from './Disclosure.svelte';

  // One row of a tree: the look every tree renderer shares (Files, SYSTEM,
  // the command browser).  The renderer keeps its own structure and
  // behaviour and passes either a plain `label` or its own `content`;
  // `trailing` takes a value or hint column.  Selection shows as the
  // focused-list colour while focus is inside the tree and as the inactive
  // colour otherwise.  The root takes `role` and the ARIA the renderer
  // needs; `class` carries legacy hooks (tree-row, sys-line, cmd-line).
  interface Props extends Omit<HTMLAttributes<HTMLDivElement>, 'content'> {
    depth: number;
    label?: string;
    content?: Snippet;
    icon?: IconName;
    description?: string;
    trailing?: Snippet;
    hasChildren?: boolean;
    open?: boolean;
    selected?: boolean;
    loading?: boolean;
    dragSource?: boolean;
    dropTarget?: boolean;
    /** A filter's verdict on the row: an exact `match` or a `dim` other. */
    filter?: 'match' | 'dim';
    variant?: 'default' | 'category' | 'placeholder';
    density?: 'row' | 'compact';
    /** What the row stands for (method, attr, …); a hook for styling. */
    kind?: string;
    /** Whether the row lights up under the pointer. */
    hover?: boolean;
    onDisclosureClick?: (ev: MouseEvent) => void;
    class?: string;
  }
  let {
    depth,
    label,
    content,
    icon,
    description,
    trailing,
    hasChildren = false,
    open = false,
    selected = false,
    loading = false,
    dragSource = false,
    dropTarget = false,
    filter,
    variant = 'default',
    density = 'row',
    kind,
    hover = true,
    onDisclosureClick,
    class: cls = '',
    ...rest
  }: Props = $props();

  const dataState = $derived(dragSource ? 'drag-source' : dropTarget ? 'drop-target' : filter);
</script>

<div
  {...rest}
  class="gs-tree-item {cls}"
  class:selected
  class:gs-tree-item--category={variant === 'category'}
  class:gs-tree-item--placeholder={variant === 'placeholder'}
  class:gs-tree-item--hover={hover}
  data-density={density}
  data-selected={selected || undefined}
  data-state={dataState}
  data-kind={kind}
  style="--depth: {depth}"
>
  {#if variant !== 'placeholder'}
    <Disclosure
      class="gs-tree-item__disclosure twistie"
      {open}
      {hasChildren}
      {loading}
      onclick={onDisclosureClick}
    />
  {/if}
  {#if icon}
    <span class="gs-tree-item__icon icon"><Icon name={icon} size="base" /></span>
  {/if}
  {#if content}
    {@render content()}
  {:else}
    <span class="gs-tree-item__label label">{label}</span>
  {/if}
  {#if description}
    <span class="gs-tree-item__description desc">{description}</span>
  {/if}
  {#if trailing}
    {@render trailing()}
  {/if}
</div>

<style>
  .gs-tree-item {
    position: relative;
    display: flex;
    align-items: center;
    gap: var(--gs-row-gap);
    padding: 0 var(--gs-row-padding-x) 0
      calc(var(--gs-tree-indent-base) + var(--gs-tree-indent) * var(--depth));
    cursor: pointer;
    color: var(--gs-text);
  }
  /* Indent guides: one vertical line per ancestor level, through the
     middle of each ancestor's disclosure arrow.  Transparent unless a skin
     sets --gs-tree-guide. */
  .gs-tree-item::before {
    content: '';
    position: absolute;
    top: 0;
    bottom: 0;
    left: calc(
      var(--gs-tree-indent-base) + var(--gs-disclosure-size) / 2 - var(--gs-tree-guide-width) / 2
    );
    width: calc(var(--gs-tree-indent) * var(--depth));
    background: repeating-linear-gradient(
      to right,
      var(--gs-tree-guide) 0 var(--gs-tree-guide-width),
      transparent var(--gs-tree-guide-width) var(--gs-tree-indent)
    );
    pointer-events: none;
  }
  .gs-tree-item[data-density='row'] {
    height: var(--gs-row-height);
    font-size: var(--gs-row-font-size);
  }
  .gs-tree-item[data-density='compact'] {
    min-height: var(--gs-row-height-compact);
    padding-top: var(--gs-row-padding-y-compact);
    padding-bottom: var(--gs-row-padding-y-compact);
  }
  .gs-tree-item--hover:hover,
  .gs-tree-item--hover[data-force-state='hover'] {
    background: var(--gs-row-hover);
  }
  .gs-tree-item[data-selected] {
    background: var(--gs-row-selected-inactive);
    color: var(--gs-row-selected-inactive-fg);
  }
  :global([role='tree']:focus-within) .gs-tree-item[data-selected] {
    background: var(--gs-row-selected);
    color: var(--gs-row-selected-fg);
  }
  .gs-tree-item[data-state='drag-source'] {
    opacity: var(--gs-opacity-drag-source);
  }
  .gs-tree-item[data-state='drop-target'] {
    outline: var(--gs-focus-width) solid var(--gs-drop-border);
    outline-offset: var(--gs-focus-offset);
    background: var(--gs-tree-drop-bg);
  }
  .gs-tree-item[data-state='dim'] {
    opacity: var(--gs-tree-dim-opacity);
  }
  .gs-tree-item[data-state='match'] :global(.gs-tree-item__label) {
    text-decoration: underline;
    text-decoration-color: var(--gs-focus-ring);
    text-underline-offset: 3px;
  }
  /* A category row heads the rows under it (which share its indent). */
  .gs-tree-item--category {
    padding-top: var(--gs-tree-category-padding-top);
    font-weight: var(--gs-tree-category-weight);
    color: var(--gs-tree-category-fg);
  }
  /* "(empty)" under an open branch with no children. */
  .gs-tree-item--placeholder {
    cursor: default;
    color: var(--gs-text-muted);
    font-style: italic;
    padding-left: calc(
      var(--gs-tree-indent-base) + var(--gs-tree-indent) * var(--depth) +
        var(--gs-disclosure-size) + var(--gs-row-gap)
    );
  }
  .gs-tree-item__icon {
    flex-shrink: 0;
    display: inline-flex;
    align-items: center;
    color: var(--gs-text-muted);
  }
  .gs-tree-item__label {
    flex: 1 1 auto;
    min-width: 0;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .gs-tree-item__description {
    color: var(--gs-text-muted);
    font-size: var(--gs-font-size-sm);
    flex-shrink: 0;
    margin-left: var(--gs-space-2);
  }
</style>
