<script lang="ts">
  // A row-shaped action: an icon and a label, the whole row clickable (the
  // Welcome card's New Machine, Open Checkpoint, Upload ROM).
  import Icon from '@/components/common/Icon.svelte';
  import type { IconName } from '@/lib/icons';

  interface Props {
    icon: IconName;
    label: string;
    description?: string;
    onclick: () => void;
    class?: string;
    /** The gallery's forced state ('hover'). */
    'data-force-state'?: string;
  }
  let {
    icon,
    label,
    description,
    onclick,
    class: cls = '',
    'data-force-state': forceState,
  }: Props = $props();
</script>

<button type="button" class="gs-action-row {cls}" data-force-state={forceState} {onclick}>
  <Icon name={icon} class="gs-action-row__icon" />
  <span class="gs-action-row__label">{label}</span>
  {#if description}<span class="gs-action-row__description">{description}</span>{/if}
</button>

<style>
  .gs-action-row {
    display: flex;
    align-items: center;
    gap: var(--gs-space-2-5);
    width: 100%;
    padding: var(--gs-space-1-5) var(--gs-space-2);
    border: none;
    border-radius: var(--gs-action-row-radius);
    background: transparent;
    color: var(--gs-action-row-fg);
    text-align: left;
    cursor: pointer;
  }
  .gs-action-row:hover,
  .gs-action-row[data-force-state='hover'] {
    background: var(--gs-action-row-bg-hover);
  }
  .gs-action-row :global(.gs-action-row__icon) {
    width: var(--gs-size-icon);
    height: var(--gs-size-icon);
    color: var(--gs-text);
  }
  .gs-action-row__description {
    margin-left: auto;
    color: var(--gs-text-muted);
  }
</style>
