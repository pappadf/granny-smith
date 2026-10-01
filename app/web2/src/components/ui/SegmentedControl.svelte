<script lang="ts" generics="V extends string | boolean">
  // A row of mutually exclusive options (the scheduler's speed, Logical /
  // Physical, S / U).  The selected option is aria-pressed, and looks
  // different from a hovered one.  `framed` draws the group's outline.
  // Legacy option classes (sch-btn, mem-mode-btn, su-btn) come through
  // `optionClass` for the tests that select them.
  interface Option {
    value: V;
    label: string;
    title?: string;
  }
  interface Props {
    options: readonly Option[];
    value: V;
    onChange: (value: V) => void;
    label: string;
    size?: 'sm' | 'md';
    framed?: boolean;
    disabled?: boolean;
    class?: string;
    optionClass?: string;
    /** A short caption a skin may show beside the control (data-caption). */
    caption?: string;
  }
  let {
    options,
    value,
    onChange,
    label,
    size = 'md',
    framed = false,
    disabled = false,
    class: cls = '',
    optionClass = '',
    caption,
  }: Props = $props();
</script>

<div
  class="gs-segmented {cls}"
  data-caption={caption}
  role="group"
  aria-label={label}
  data-size={size}
  data-framed={framed || undefined}
>
  {#each options as o (String(o.value))}
    <button
      type="button"
      class="gs-segmented__option {optionClass}"
      class:active={o.value === value}
      aria-pressed={o.value === value}
      title={o.title}
      {disabled}
      onclick={() => onChange(o.value)}>{o.label}</button
    >
  {/each}
</div>

<style>
  /* One line of labels, never squeezed into two: a toolbar short of room
     shrinks its gaps, not this. */
  .gs-segmented {
    display: inline-flex;
    flex: none;
    align-items: stretch;
    border-radius: var(--gs-segmented-radius);
    overflow: hidden;
  }
  .gs-segmented[data-framed] {
    border: var(--gs-border-width) solid var(--gs-border);
    border-radius: var(--gs-radius-xs);
    height: var(--gs-size-control);
  }
  .gs-segmented__option {
    padding: var(--gs-space-0-5) var(--gs-space-1-5);
    border: none;
    border-radius: var(--gs-segmented-radius);
    background: transparent;
    color: var(--gs-segmented-fg);
    font-size: var(--gs-segmented-font-size);
    white-space: nowrap;
    cursor: pointer;
  }
  .gs-segmented[data-framed] .gs-segmented__option {
    padding: 0 var(--gs-space-2);
    border-radius: 0;
  }
  .gs-segmented__option:hover:not(:disabled) {
    color: var(--gs-segmented-fg-hover);
    background: var(--gs-segmented-bg-hover);
  }
  .gs-segmented__option[aria-pressed='true'] {
    color: var(--gs-segmented-fg-selected);
    background: var(--gs-segmented-bg-selected);
  }
  .gs-segmented__option:disabled {
    opacity: var(--gs-opacity-disabled);
    cursor: default;
  }
  /* An unselected option has no fill, so it fades by its text colour: text
     faded through opacity alone is antialiased differently from one paint
     to the next. */
  .gs-segmented__option:disabled:not([aria-pressed='true']) {
    opacity: 1;
    color: var(--gs-segmented-fg-disabled);
  }
</style>
