<script lang="ts">
  // One form row: a label, its control, and a help or error line under the
  // control.  `for` ties the label to the control's id; without a control
  // (help alone) the row is just its text in the control column.
  import type { Snippet } from 'svelte';

  interface Props {
    label?: string;
    for?: string;
    help?: string;
    error?: string;
    // Stacked: the label above the control (dialogs), not beside it.
    stacked?: boolean;
    class?: string;
    children?: Snippet;
  }
  let {
    label,
    for: forId,
    help,
    error,
    stacked = false,
    class: cls = '',
    children,
  }: Props = $props();
</script>

<div class="gs-field {cls}" data-stacked={stacked || undefined}>
  {#if forId}
    <label class="gs-field__label" for={forId}>{label ?? ''}</label>
  {:else}
    <span class="gs-field__label form-label">{label ?? ''}</span>
  {/if}
  <div class="gs-field__control">
    {@render children?.()}
    {#if help}<div class="gs-field__help form-help">{help}</div>{/if}
    {#if error}<div class="gs-field__error" role="alert">{error}</div>{/if}
  </div>
</div>

<style>
  .gs-field {
    display: grid;
    grid-template-columns: var(--gs-form-label-width) 1fr;
    align-items: center;
    gap: var(--gs-space-3);
  }
  .gs-field[data-stacked] {
    grid-template-columns: 1fr;
    gap: var(--gs-space-1-5);
  }
  .gs-field__label {
    color: var(--gs-field-label-fg);
  }
  .gs-field[data-stacked] .gs-field__label {
    font-size: var(--gs-font-size-sm);
    color: var(--gs-field-help-fg);
  }
  .gs-field__control {
    display: flex;
    flex-direction: column;
    gap: var(--gs-space-1);
    min-width: 0;
  }
  .gs-field__help {
    color: var(--gs-field-help-fg);
    font-size: var(--gs-font-size-sm);
    line-height: var(--gs-line-height-base);
  }
  .gs-field__error {
    color: var(--gs-field-error-fg);
    font-size: var(--gs-font-size-sm);
  }
</style>
