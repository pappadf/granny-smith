<script lang="ts">
  import { iconHref, type IconName } from '@/lib/icons';

  interface Props {
    name: IconName;
    size?: number;
    class?: string;
    title?: string;
  }
  let { name, size = 16, class: cls = '', title }: Props = $props();
</script>

<svg
  width={size}
  height={size}
  class={'icon ' + cls}
  aria-hidden={title ? undefined : true}
  role={title ? 'img' : undefined}
>
  {#if title}<title>{title}</title>{/if}
  <use href={iconHref(name)} />
</svg>

<style>
  .icon {
    fill: currentColor;
    flex-shrink: 0;
  }
</style>
