<script lang="ts">
  import { onMount } from 'svelte';
  import Tree, { type TreeNode } from '@/components/common/Tree.svelte';
  import { loadMachineRoots, loadMachineChildren } from '@/bus/machineTree';
  import { machine } from '@/state/machine.svelte';

  let rootNodes = $state<TreeNode[]>([]);
  let expanded = $state<Record<string, boolean>>({});
  let loading = $state(true);

  async function refresh() {
    loading = true;
    rootNodes = (await loadMachineRoots()) as TreeNode[];
    loading = false;
  }

  onMount(() => {
    void refresh();
  });

  // Re-scan when the machine boots / shuts down so the tree reflects the
  // currently registered subtrees (different models expose different
  // subtrees).
  $effect(() => {
    void machine.status;
    void refresh();
  });

  async function loadChildren(path: string[]): Promise<TreeNode[]> {
    const kids = await loadMachineChildren(path);
    return kids as TreeNode[];
  }
</script>

<div class="machine-view">
  {#if loading}
    <p class="hint">Loading machine tree…</p>
  {:else if rootNodes.length === 0}
    <p class="hint">No machine is running yet. Start one from the Welcome view.</p>
  {:else}
    <Tree
      nodes={rootNodes}
      {expanded}
      onToggle={(p) => {
        const k = p.join(' ');
        expanded[k] = !expanded[k];
      }}
      {loadChildren}
    />
  {/if}
</div>

<style>
  .machine-view {
    width: 100%;
    height: 100%;
    overflow: auto;
    background: var(--gs-bg);
    padding: 4px 0;
  }
  .hint {
    color: var(--gs-fg-muted);
    font-size: 12px;
    padding: 16px;
    line-height: 1.5;
  }
</style>
