<script lang="ts">
  // The Breakpoints section: a DebugPointList whose add form takes an
  // optional condition and whose rows show the MMU-translated address.
  import DebugPointList from './DebugPointList.svelte';
  import { listBreakpoints, addBreakpoint, removeBreakpoint, type Breakpoint } from '@/bus/debug';
  import { machine } from '@/state/machine.svelte';
  import { debug, bumpBreakpoints } from '@/state/debug.svelte';
  import { translateMany, addrLabel, type Translation } from '@/bus/mmu';
  import { fmtHex32 } from '@/lib/hex';
  import TextInput from '@/components/ui/TextInput.svelte';

  let rows = $state<Breakpoint[]>([]);
  let addCond = $state('');

  // A condition that does not parse is refused by the core, with its
  // reason; the form stays open to correct it.
  async function add(addr: number): Promise<true | string> {
    const added = await addBreakpoint(addr, addCond.trim() || undefined);
    return added === true ? true : `Failed to add breakpoint: ${added}`;
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

<!-- Also refreshed after a breakpoint is added or removed elsewhere (the
     Disassembly pane's context menu): debug.breakpointsGen. -->
<DebugPointList
  title="Breakpoints"
  noun="breakpoint"
  section="breakpoints"
  rowClass="bp-row"
  list={listBreakpoints}
  {add}
  remove={removeBreakpoint}
  onChanged={bumpBreakpoints}
  gen={() => debug.breakpointsGen}
  onReset={() => (addCond = '')}
  bind:rows
>
  {#snippet fields(keydown: (ev: KeyboardEvent) => void)}
    <TextInput
      class="add-cond"
      mono
      style="flex: 1 1 auto"
      placeholder="condition (optional)"
      bind:value={addCond}
      onkeydown={keydown}
      aria-label="Breakpoint condition"
    />
  {/snippet}
  {#snippet label(r: Breakpoint)}
    <span class="addr">{labelFor(r.addr)}</span>
    {#if r.condition}
      <span class="cond">if {r.condition}</span>
    {/if}
  {/snippet}
</DebugPointList>

<style>
  .cond {
    color: var(--gs-text-muted);
    font-style: italic;
  }
</style>
