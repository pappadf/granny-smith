<script lang="ts">
  import Icon from '@/components/common/Icon.svelte';
  import type { IconName } from '@/lib/icons';

  // Every sprite id the registry names.
  const names: IconName[] = [
    'play',
    'pause',
    'stop',
    'minus',
    'plus',
    'download',
    'layout-left',
    'layout-left-off',
    'layout-bottom',
    'layout-bottom-off',
    'layout-right',
    'layout-right-off',
    'close',
    'chevron',
    'step-into',
    'step-over',
    'restart',
    'trash',
    'chip',
    'hd',
    'floppy',
    'cd',
    'computer',
    'folder',
    'file',
    'speaker',
    'port',
    'clock',
    'empty',
    'upload',
    'mac',
    'sign-out',
    'screen-full',
    'screen-normal',
    'camera',
    'camera-off',
    'mic',
    'mic-off',
    'ellipsis',
    'power',
    'brush',
  ];
</script>

<div class="grid">
  {#each names as n (n)}
    <span class="cell" title={n}><Icon name={n} /></span>
  {/each}
</div>

<style>
  .grid {
    display: grid;
    grid-template-columns: repeat(13, 32px);
    gap: var(--gs-space-2);
    padding: var(--gs-space-3);
  }
  .cell {
    display: inline-flex;
    align-items: center;
    justify-content: center;
    height: 32px;
  }
</style>
