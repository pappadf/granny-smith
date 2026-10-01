<script lang="ts">
  // A text link.  `external` opens a new tab safely.  A link that acts in the
  // page (Back) passes onclick and no href.
  import type { Snippet } from 'svelte';
  import type { HTMLAnchorAttributes } from 'svelte/elements';
  import Icon from '@/components/common/Icon.svelte';
  import type { IconName } from '@/lib/icons';

  interface Props extends Omit<HTMLAnchorAttributes, 'children'> {
    external?: boolean;
    icon?: IconName;
    children: Snippet;
  }
  let { external = false, icon, class: cls = '', href, children, ...rest }: Props = $props();
</script>

<a
  class="gs-link {cls}"
  href={href ?? '#'}
  target={external ? '_blank' : undefined}
  rel={external ? 'noopener noreferrer' : undefined}
  {...rest}
>
  {#if icon}<Icon name={icon} size={12} class="gs-link__icon" />{/if}{@render children()}
</a>

<style>
  .gs-link {
    display: inline-flex;
    align-items: center;
    gap: var(--gs-space-1);
    color: var(--gs-link-fg);
    text-decoration: none;
    cursor: pointer;
  }
  .gs-link:hover {
    text-decoration: underline;
  }
  .gs-link:visited {
    color: var(--gs-link-fg);
  }
</style>
