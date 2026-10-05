<script lang="ts">
  import Button from '@/components/ui/Button.svelte';
  import IconButton from '@/components/ui/IconButton.svelte';
  import ActionRow from '@/components/ui/ActionRow.svelte';
  import ListRow from '@/components/ui/ListRow.svelte';
  import TreeItem from '@/components/ui/TreeItem.svelte';

  // Hover and active forced with data-force-state (a gallery-only hook the
  // primitives honour), so every pointer state is in one screenshot.
  const variants = ['primary', 'secondary', 'danger', 'ghost'] as const;
  const forced = ['', 'hover', 'active'] as const;
  const noop = () => undefined;
</script>

<div class="grid">
  {#each forced as f (f)}
    <div class="row">
      <span class="lbl">{f || 'rest'}</span>
      {#each variants as v (v)}
        <Button variant={v} data-force-state={f || undefined}>{v}</Button>
      {/each}
      <IconButton icon="play" label="toolbar" data-force-state={f || undefined} />
      <IconButton icon="restart" label="panel" tone="panel" data-force-state={f || undefined} />
    </div>
  {/each}
  <div class="rows">
    <ActionRow icon="mac" label="Action row (hover)" onclick={noop} data-force-state="hover" />
    <ListRow data-force-state="hover"><span>List row (hover)</span></ListRow>
    <TreeItem depth={1} label="Tree item (hover)" icon="folder" data-force-state="hover" />
  </div>
</div>

<style>
  .grid {
    display: flex;
    flex-direction: column;
    gap: var(--gs-space-2);
    padding: var(--gs-space-3);
  }
  .row {
    display: flex;
    align-items: center;
    gap: var(--gs-space-2);
  }
  .lbl {
    width: 48px;
    font-size: var(--gs-font-size-xs);
    color: var(--gs-text-muted);
  }
  .rows {
    width: 320px;
  }
</style>
