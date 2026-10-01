<script lang="ts">
  import { onMount } from 'svelte';
  import type { DriveActivity } from '@/state/machine.svelte';
  import { readToken } from '@/lib/tokens';

  // A status-bar drive light: the drive's short name (HD, FD, CD, CP), dim
  // when idle and lit in the read or write colour.  A skin may switch the
  // look with --gs-statusbar-drive-style (text, icon or led), exposed as
  // data-style.  Legacy hooks: sb-item, sb-drive, drive-ico.
  interface Props {
    label: string;
    title: string;
    activity: DriveActivity;
  }
  let { label, title, activity }: Props = $props();

  let style = $state('text');
  onMount(() => {
    style = readToken('--gs-statusbar-drive-style') || 'text';
  });
</script>

<div
  class="gs-drive gs-statusbar__item sb-item sb-drive"
  class:active-read={activity === 'read'}
  class:active-write={activity === 'write'}
  data-activity={activity}
  data-style={style}
  {title}
>
  <span class="gs-drive__icon drive-ico">{label}</span>
</div>

<style>
  .gs-drive {
    display: flex;
    align-items: center;
    padding: 0 var(--gs-space-2);
    opacity: var(--gs-statusbar-drive-idle-opacity);
    transition:
      opacity var(--gs-duration-fast),
      color var(--gs-duration-fast);
  }
  .gs-drive[data-activity='read'] {
    opacity: 1;
    color: var(--gs-statusbar-drive-read);
  }
  .gs-drive[data-activity='write'] {
    opacity: 1;
    color: var(--gs-statusbar-drive-write);
  }
  .gs-drive__icon {
    display: inline-block;
    font-family: var(--gs-font-mono);
    font-size: var(--gs-font-size-xs);
    font-weight: var(--gs-font-weight-semibold);
  }
  /* The led style: a lamp in place of the name (kept for screen readers). */
  .gs-drive[data-style='led'] .gs-drive__icon {
    width: var(--gs-size-dot);
    height: var(--gs-size-dot);
    border-radius: var(--gs-radius-round);
    background: currentColor;
    overflow: hidden;
    color: transparent;
  }
  .gs-drive[data-style='led'] {
    color: var(--gs-text-subtle);
  }
</style>
