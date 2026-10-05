<script lang="ts">
  import Icon from '@/components/common/Icon.svelte';
  import Hint from '@/components/ui/Hint.svelte';
  import ListRow from '@/components/ui/ListRow.svelte';
  // The Watchpoints section: debug.watchpoints, a memory logpoint that stops
  // the machine after the accessing instruction (#180).  Same shape as the
  // Breakpoints section; a row is an address range, its access mode and hits.
  import { onMount } from 'svelte';
  import CollapsibleSection from '@/components/common/CollapsibleSection.svelte';
  import { openContextMenu, type ContextMenuItem } from '@/components/common/ContextMenu.svelte';
  import { listWatchpoints, addWatchpoint, removeWatchpoint, type Watchpoint } from '@/bus/debug';
  import { machine } from '@/state/machine.svelte';
  import { showNotification } from '@/state/toasts.svelte';
  import { debug, toggleSection } from '@/state/debug.svelte';
  import { fmtHex32, parseHex } from '@/lib/hex';
  import Button from '@/components/ui/Button.svelte';
  import IconButton from '@/components/ui/IconButton.svelte';
  import TextInput from '@/components/ui/TextInput.svelte';
  import Select from '@/components/ui/Select.svelte';

  let rows = $state<Watchpoint[]>([]);
  let showAdd = $state(false);
  let addAddr = $state('');
  let addMode = $state<'write' | 'read' | 'rw'>('write');
  let addWidth = $state<'' | 'b' | 'w' | 'l'>('l');

  // Only the latest listing is shown (see BreakpointsSection).
  let listGen = 0;
  async function refresh() {
    const gen = ++listGen;
    const list = await listWatchpoints();
    if (gen === listGen) rows = list;
  }

  onMount(() => {
    void refresh();
  });

  $effect(() => {
    void machine.status;
    if (debug.sections.watchpoints) void refresh();
  });

  function onAddrKey(ev: KeyboardEvent) {
    if (ev.key === 'Enter') {
      ev.preventDefault();
      void commitAdd();
    } else if (ev.key === 'Escape') {
      cancelAdd();
    }
  }

  async function commitAdd() {
    const v = parseHex(addAddr);
    if (v === null) {
      showNotification('Invalid watchpoint address', 'error');
      return;
    }
    const ok = await addWatchpoint(v, addMode, addWidth);
    if (!ok) {
      showNotification('Failed to add watchpoint', 'error');
      return;
    }
    showNotification(`Watchpoint set at $${fmtHex32(v)}`, 'info');
    addAddr = '';
    showAdd = false;
    await refresh();
  }

  function cancelAdd() {
    addAddr = '';
    showAdd = false;
  }

  function onRowContext(row: Watchpoint, ev: MouseEvent) {
    ev.preventDefault();
    const items: ContextMenuItem[] = [
      {
        label: 'Remove',
        danger: true,
        action: () => void doRemove(row),
      },
    ];
    openContextMenu(items, ev.clientX, ev.clientY);
  }

  async function doRemove(row: Watchpoint) {
    const ok = await removeWatchpoint(row.id);
    if (!ok) {
      showNotification('Failed to remove watchpoint', 'error');
      return;
    }
    await refresh();
  }

  // "$0000016A" for one address, "$0000016A-$0000016D" for a range.
  function rangeLabel(r: Watchpoint): string {
    return r.end !== r.addr ? `$${fmtHex32(r.addr)}-$${fmtHex32(r.end)}` : `$${fmtHex32(r.addr)}`;
  }
</script>

<CollapsibleSection
  title="Watchpoints"
  open={debug.sections.watchpoints}
  onToggle={() => toggleSection('watchpoints')}
>
  {#snippet actions()}
    <IconButton
      class="add-btn"
      icon="plus"
      size="sm"
      tone="panel"
      rest="faded"
      iconSize="md"
      label="Add watchpoint"
      onclick={() => (showAdd = true)}
    />
  {/snippet}
  {#if showAdd}
    <div class="add-row">
      <TextInput
        class="add-addr"
        mono
        widthCh={12}
        placeholder="address ($hex)"
        bind:value={addAddr}
        onkeydown={onAddrKey}
        aria-label="Watchpoint address"
      />
      <Select
        class="add-mode"
        size="sm"
        style="flex: 1 1 auto"
        bind:value={addMode}
        aria-label="Watchpoint access"
      >
        <option value="write">write</option>
        <option value="read">read</option>
        <option value="rw">read/write</option>
      </Select>
      <Select
        class="add-mode"
        size="sm"
        style="flex: 1 1 auto"
        bind:value={addWidth}
        aria-label="Watchpoint width"
      >
        <option value="b">byte</option>
        <option value="w">word</option>
        <option value="l">long</option>
      </Select>
      <Button variant="primary" class="btn" onclick={commitAdd}>Add</Button>
      <Button class="btn" onclick={cancelAdd}>Cancel</Button>
    </div>
  {/if}
  {#if rows.length === 0 && !showAdd}
    <Hint class="hint" inset="section">No watchpoints. Click + to add one.</Hint>
  {:else}
    {#each rows as r (r.id)}
      <ListRow class="wp-row" density="compact" mono oncontextmenu={(ev) => onRowContext(r, ev)}>
        <span class="enable" data-state={r.enabled ? 'on' : 'off'}
          ><Icon name={r.enabled ? 'circle-filled' : 'circle-outline'} size="xs" /></span
        >
        <span class="addr">{rangeLabel(r)}</span>
        <span class="mode">{r.mode}</span>
        {#if r.hits > 0}
          <span class="hits">{r.hits}×</span>
        {/if}
      </ListRow>
    {/each}
  {/if}
</CollapsibleSection>

<style>
  .add-row {
    display: flex;
    gap: var(--gs-space-1-5);
    padding: var(--gs-space-1-5) var(--gs-space-3);
  }
  .enable {
    display: inline-flex;
    color: var(--gs-code-breakpoint-off);
  }
  .enable[data-state='on'] {
    color: var(--gs-code-breakpoint);
  }
  .mode {
    color: var(--gs-text-muted);
    font-style: italic;
  }
  .hits {
    color: var(--gs-text-muted);
  }
</style>
