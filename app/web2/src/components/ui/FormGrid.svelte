<script lang="ts">
  // A form laid out as rows of Fields: labels in one column of
  // --gs-form-label-width.  `class` carries legacy hooks (config-form).
  import type { Snippet } from 'svelte';
  import type { HTMLFormAttributes } from 'svelte/elements';

  interface Props extends Omit<HTMLFormAttributes, 'children'> {
    children: Snippet;
  }
  let { class: cls = '', children, ...rest }: Props = $props();
</script>

<form class="gs-form {cls}" {...rest}>
  {@render children()}
</form>

<style>
  .gs-form {
    display: flex;
    flex-direction: column;
    gap: var(--gs-form-gap);
  }
</style>
