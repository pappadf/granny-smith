<script lang="ts">
  import type { DriveActivity } from '@/state/machine.svelte';

  interface Props {
    label: string;
    title: string;
    activity: DriveActivity;
  }
  let { label, title, activity }: Props = $props();
</script>

<div
  class="sb-item sb-drive"
  class:active-read={activity === 'read'}
  class:active-write={activity === 'write'}
  {title}
>
  <span class="drive-ico">{label}</span>
</div>

<style>
  .sb-item {
    display: flex;
    align-items: center;
    gap: var(--gs-space-1);
    padding: 0 var(--gs-space-2);
    cursor: pointer;
  }
  .sb-item:hover {
    background: var(--gs-state-hover);
  }
  .sb-drive {
    opacity: 0.55;
    transition:
      opacity var(--gs-duration-fast),
      color var(--gs-duration-fast);
  }
  .sb-drive.active-read {
    opacity: 1;
    color: var(--gs-statusbar-drive-read);
  }
  .sb-drive.active-write {
    opacity: 1;
    color: var(--gs-statusbar-drive-write);
  }
  .drive-ico {
    display: inline-block;
    font-family: var(--gs-font-mono);
    font-size: var(--gs-font-size-xs);
    font-weight: var(--gs-font-weight-semibold);
  }
</style>
