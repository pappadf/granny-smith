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
    gap: 4px;
    padding: 0 8px;
    cursor: pointer;
  }
  .sb-item:hover {
    background: var(--gs-sb-hover);
  }
  .sb-drive {
    opacity: 0.55;
    transition:
      opacity 0.1s,
      color 0.1s;
  }
  .sb-drive.active-read {
    opacity: 1;
    color: #ffffff;
  }
  .sb-drive.active-write {
    opacity: 1;
    color: #ffe770;
  }
  .drive-ico {
    display: inline-block;
    font-family: var(--gs-font-mono);
    font-size: 11px;
    font-weight: 600;
  }
</style>
