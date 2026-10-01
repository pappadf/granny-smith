<script lang="ts">
  import type { HTMLButtonAttributes } from 'svelte/elements';

  // An on/off switch (role=switch): a pill track with a sliding knob.
  // `class` carries the legacy hook (toggle); `on` is kept as a class.
  interface Props extends Omit<HTMLButtonAttributes, 'onchange'> {
    checked: boolean;
    label: string;
    onchange?: (checked: boolean) => void;
    class?: string;
  }
  let { checked, label, onchange, class: cls = '', ...rest }: Props = $props();
</script>

<button
  type="button"
  {...rest}
  class="gs-switch {cls}"
  class:on={checked}
  role="switch"
  aria-checked={checked}
  aria-label={label}
  onclick={() => onchange?.(!checked)}
>
  <span class="gs-switch__knob knob"></span>
</button>

<style>
  .gs-switch {
    position: relative;
    flex: none;
    width: var(--gs-switch-width);
    height: var(--gs-switch-height);
    border-radius: var(--gs-radius-pill);
    border: var(--gs-border-width) solid var(--gs-switch-border);
    background: var(--gs-switch-track);
    padding: 0;
    cursor: pointer;
  }
  .gs-switch[aria-checked='true'] {
    background: var(--gs-switch-track-on);
  }
  .gs-switch:disabled {
    cursor: default;
    opacity: var(--gs-switch-disabled-opacity);
  }
  .gs-switch:focus-visible {
    outline: var(--gs-focus-width) solid var(--gs-focus-ring);
    outline-offset: 1px;
  }
  .gs-switch__knob {
    position: absolute;
    top: var(--gs-switch-inset);
    left: var(--gs-switch-inset);
    width: var(--gs-switch-knob-size);
    height: var(--gs-switch-knob-size);
    border-radius: var(--gs-radius-round);
    background: var(--gs-switch-knob);
    transition: left var(--gs-duration-fast);
  }
  .gs-switch[aria-checked='true'] > .gs-switch__knob {
    left: calc(
      var(--gs-switch-width) - 2 * var(--gs-border-width) - var(--gs-switch-inset) -
        var(--gs-switch-knob-size)
    );
    background: var(--gs-switch-knob-on);
  }
</style>
