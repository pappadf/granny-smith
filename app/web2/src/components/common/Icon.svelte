<script lang="ts" module>
  // Named icon sizes, each a --gs-size-icon-* token: xs 12, sm 13, md 14,
  // base 16 (the sprite's glyphs are drawn for 16).
  export type IconSize = 'xs' | 'sm' | 'md' | 'base';
</script>

<script lang="ts">
  import { iconHref, type IconName } from '@/lib/icons';

  interface Props {
    name: IconName;
    size?: IconSize;
    class?: string;
    title?: string;
  }
  let { name, size = 'base', class: cls = '', title }: Props = $props();
</script>

<svg
  class={'icon ' + cls}
  data-size={size}
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
    width: var(--gs-size-icon);
    height: var(--gs-size-icon);
  }
  .icon[data-size='xs'] {
    width: var(--gs-size-icon-xs);
    height: var(--gs-size-icon-xs);
  }
  .icon[data-size='sm'] {
    width: var(--gs-size-icon-sm);
    height: var(--gs-size-icon-sm);
  }
  .icon[data-size='md'] {
    width: var(--gs-size-icon-md);
    height: var(--gs-size-icon-md);
  }
</style>
