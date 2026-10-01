<script lang="ts">
  // An indeterminate busy ring.  `tone="current"` draws it in the text
  // colour with an open gap (inside a button or a row); `accent` draws an
  // accent arc on a subtle track.  Without a `label` it is decorative.
  interface Props {
    size?: 'sm' | 'md';
    tone?: 'accent' | 'current';
    label?: string;
    class?: string;
  }
  let { size = 'md', tone = 'accent', label, class: cls = '' }: Props = $props();
</script>

<span
  class="gs-spinner {cls}"
  data-size={size}
  data-tone={tone}
  role={label ? 'status' : undefined}
  aria-label={label}
  aria-hidden={label ? undefined : true}
></span>

<style>
  .gs-spinner {
    display: inline-block;
    flex: none;
    width: var(--gs-spinner-size);
    height: var(--gs-spinner-size);
    border-radius: var(--gs-radius-round);
    border: var(--gs-spinner-width) solid var(--gs-spinner-track);
    border-top-color: var(--gs-spinner-fg);
    animation: gs-spin var(--gs-duration-spin) var(--gs-ease-linear) infinite;
  }
  .gs-spinner[data-size='sm'] {
    width: var(--gs-spinner-size-sm);
    height: var(--gs-spinner-size-sm);
  }
  .gs-spinner[data-tone='current'] {
    border-color: currentColor;
    border-right-color: transparent;
  }
  @keyframes gs-spin {
    to {
      transform: rotate(360deg);
    }
  }
</style>
