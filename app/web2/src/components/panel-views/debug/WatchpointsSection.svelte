<script lang="ts">
  // The Watchpoints section: debug.watchpoints, a memory logpoint that stops
  // the machine after the accessing instruction (#180).  A DebugPointList
  // like the Breakpoints section; a row is an address range, its access mode
  // and hits.
  import DebugPointList from './DebugPointList.svelte';
  import { listWatchpoints, addWatchpoint, removeWatchpoint, type Watchpoint } from '@/bus/debug';
  import { fmtHex32 } from '@/lib/hex';
  import Select from '@/components/ui/Select.svelte';

  let addMode = $state<'write' | 'read' | 'rw'>('write');
  let addWidth = $state<'' | 'b' | 'w' | 'l'>('l');

  async function add(addr: number): Promise<true | string> {
    return (await addWatchpoint(addr, addMode, addWidth)) ? true : 'Failed to add watchpoint';
  }

  // "$0000016A" for one address, "$0000016A-$0000016D" for a range.
  function rangeLabel(r: Watchpoint): string {
    return r.end !== r.addr ? `$${fmtHex32(r.addr)}-$${fmtHex32(r.end)}` : `$${fmtHex32(r.addr)}`;
  }
</script>

<DebugPointList
  title="Watchpoints"
  noun="watchpoint"
  section="watchpoints"
  rowClass="wp-row"
  list={listWatchpoints}
  {add}
  remove={removeWatchpoint}
>
  {#snippet fields()}
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
  {/snippet}
  {#snippet label(r: Watchpoint)}
    <span class="addr">{rangeLabel(r)}</span>
    <span class="mode">{r.mode}</span>
  {/snippet}
</DebugPointList>

<style>
  .mode {
    color: var(--gs-text-muted);
    font-style: italic;
  }
</style>
