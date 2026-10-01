<script lang="ts">
  // A number field: a TextInput of type number.  The spinner buttons stay
  // native and follow the colour scheme.
  import type { HTMLInputAttributes } from 'svelte/elements';
  import TextInput from './TextInput.svelte';

  interface Props extends Omit<HTMLInputAttributes, 'size' | 'value' | 'type'> {
    value?: number | string | null;
    size?: 'inline' | 'sm' | 'lg';
    invalid?: boolean;
    widthCh?: number;
    ref?: HTMLInputElement | null;
  }
  let { value = $bindable(''), ref = $bindable(null), ...rest }: Props = $props();
</script>

<TextInput type="number" bind:value bind:ref {...rest} />
