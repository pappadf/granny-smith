<script lang="ts" generics="V extends string | number">
  // A set of radio buttons, one choice of several.
  interface Option {
    value: V;
    label: string;
    disabled?: boolean;
  }
  interface Props {
    options: readonly Option[];
    value?: V;
    name: string;
    label: string;
    disabled?: boolean;
    class?: string;
  }
  let {
    options,
    value = $bindable(),
    name,
    label,
    disabled = false,
    class: cls = '',
  }: Props = $props();
</script>

<fieldset class="gs-radio-group {cls}" {disabled}>
  <legend class="gs-radio-group__legend">{label}</legend>
  {#each options as o (o.value)}
    <label class="gs-radio">
      <input type="radio" {name} value={o.value} bind:group={value} disabled={o.disabled} />
      <span class="gs-radio__label">{o.label}</span>
    </label>
  {/each}
</fieldset>

<style>
  .gs-radio-group {
    display: flex;
    flex-direction: column;
    gap: var(--gs-space-2);
    margin: 0;
    padding: 0;
    border: none;
    min-width: 0;
  }
  /* The group is named for assistive tech; the dialog's text says the rest. */
  .gs-radio-group__legend {
    position: absolute;
    width: 1px;
    height: 1px;
    overflow: hidden;
    clip-path: inset(50%);
  }
  .gs-radio {
    display: inline-flex;
    align-items: center;
    gap: var(--gs-space-2);
    cursor: pointer;
  }
  .gs-radio input {
    margin: 0;
  }
</style>
