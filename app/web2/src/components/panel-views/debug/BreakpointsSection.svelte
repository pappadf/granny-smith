<script lang="ts">
  import Icon from '@/components/common/Icon.svelte';
  import Hint from '@/components/ui/Hint.svelte';
  import ListRow from '@/components/ui/ListRow.svelte';
  import { onMount } from 'svelte';
  import CollapsibleSection from '@/components/common/CollapsibleSection.svelte';
  import { openContextMenu, type ContextMenuItem } from '@/components/common/ContextMenu.svelte';
  import { listBreakpoints, addBreakpoint, removeBreakpoint, type Breakpoint } from '@/bus/debug';
  import { machine } from '@/state/machine.svelte';
  import { showNotification } from '@/state/toasts.svelte';
  import { debug, toggleSection } from '@/state/debug.svelte';
  import { translateMany, addrLabel, type Translation } from '@/bus/mmu';
  import { fmtHex32, parseHex } from '@/lib/hex';
  import Button from '@/components/ui/Button.svelte';
  import IconButton from '@/components/ui/IconButton.svelte';
  import TextInput from '@/components/ui/TextInput.svelte';

  let rows = $state<Breakpoint[]>([]);
  let showAdd = $state(false);
  let addAddr = $state('');
  let addCond = $state('');

  // Only the latest listing is shown.  A listing is several round trips, so
  // one started before a remove (the refresh after a repeated add, say) can
  // finish after the remove's own refresh and would put the row back.
  let listGen = 0;
  async function refresh() {
    const gen = ++listGen;
    const list = await listBreakpoints();
    if (gen === listGen) rows = list;
  }

  onMount(() => {
    void refresh();
  });

  $effect(() => {
    void machine.status;
    if (debug.sections.breakpoints) void refresh();
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
      showNotification('Invalid breakpoint address', 'error');
      return;
    }
    const ok = await addBreakpoint(v, addCond.trim() || undefined);
    if (!ok) {
      showNotification('Failed to add breakpoint', 'error');
      return;
    }
    showNotification(`Breakpoint set at $${fmtHex32(v)}`, 'info');
    addAddr = '';
    addCond = '';
    showAdd = false;
    await refresh();
  }

  function cancelAdd() {
    addAddr = '';
    addCond = '';
    showAdd = false;
  }

  function onRowContext(row: Breakpoint, ev: MouseEvent) {
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

  async function doRemove(row: Breakpoint) {
    const ok = await removeBreakpoint(row.id);
    if (!ok) {
      showNotification('Failed to remove breakpoint', 'error');
      return;
    }
    await refresh();
  }

  // Real translations for the L:/P: labels (the core's, bus/mmu.ts).
  let xl = $state<Record<number, Translation>>({});
  $effect(() => {
    const addrs = rows.map((r) => r.addr);
    if (!machine.mmuEnabled || addrs.length === 0) return;
    void translateMany(addrs).then((m) => (xl = m));
  });

  function labelFor(addr: number): string {
    if (!machine.mmuEnabled) return `$${fmtHex32(addr)}`;
    return addrLabel(addr, xl[addr >>> 0]);
  }
</script>

<CollapsibleSection
  title="Breakpoints"
  open={debug.sections.breakpoints}
  onToggle={() => toggleSection('breakpoints')}
>
  {#snippet actions()}
    <IconButton
      class="add-btn"
      icon="plus"
      size="sm"
      tone="panel"
      rest="faded"
      iconSize="md"
      label="Add breakpoint"
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
        aria-label="Breakpoint address"
      />
      <TextInput
        class="add-cond"
        mono
        style="flex: 1 1 auto"
        placeholder="condition (optional)"
        bind:value={addCond}
        onkeydown={onAddrKey}
        aria-label="Breakpoint condition"
      />
      <Button variant="primary" class="btn" onclick={commitAdd}>Add</Button>
      <Button class="btn" onclick={cancelAdd}>Cancel</Button>
    </div>
  {/if}
  {#if rows.length === 0 && !showAdd}
    <Hint class="hint" inset="section">No breakpoints. Click + to add one.</Hint>
  {:else}
    {#each rows as r (r.id)}
      <ListRow class="bp-row" density="compact" mono oncontextmenu={(ev) => onRowContext(r, ev)}>
        <span class="enable" data-state={r.enabled ? 'on' : 'off'}
          ><Icon name={r.enabled ? 'circle-filled' : 'circle-outline'} size="xs" /></span
        >
        <span class="addr">{labelFor(r.addr)}</span>
        {#if r.condition}
          <span class="cond">if {r.condition}</span>
        {/if}
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
  .cond {
    color: var(--gs-text-muted);
    font-style: italic;
  }
  .hits {
    color: var(--gs-text-muted);
  }
</style>
