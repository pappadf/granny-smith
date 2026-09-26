<script lang="ts">
  // One auxiliary core (capabilities.aux_cpus — the AV family's DSP3210) in
  // the Debug view.  It answers the same frame as the main CPU
  // (machine.<name>.frame), so this renders it with the shared register
  // layout and no per-core code: run state, registers, the floating-point
  // block and a disassembly window around its PC.  Read-only — the core
  // exposes no register setters.
  import CollapsibleSection from '@/components/common/CollapsibleSection.svelte';
  import { gsEval } from '@/bus/emulator';
  import { loadDebugFrame, type DebugFrame } from '@/bus/debug';
  import { machine, type AuxCpu } from '@/state/machine.svelte';
  import { debug } from '@/state/debug.svelte';
  import { registerGroups, fmtRegister } from '@/lib/registerLayout';
  import { fmtHex32 } from '@/lib/hex';

  interface Props {
    cpu: AuxCpu;
  }
  let { cpu }: Props = $props();

  // Rows in the window, and how many precede the PC.
  const ROWS = 8;
  const BEFORE = 2;

  let frame = $state<DebugFrame | null>(null);
  let runState = $state<string | null>(null);
  let loading = $state(false);

  const open = $derived(debug.auxOpen[cpu.name] === true);
  const groups = $derived(frame ? registerGroups(frame.arch, frame.rawRegs) : []);
  const title = $derived(`${cpu.name.toUpperCase()} (${cpu.arch})`);

  // Fetch when open and paused, and again after every step.
  $effect(() => {
    void debug.refreshGen;
    if (!open || machine.status !== 'paused') {
      if (machine.status !== 'paused') frame = null;
      return;
    }
    void refresh();
  });

  async function refresh(): Promise<void> {
    loading = true;
    try {
      frame = await loadDebugFrame(undefined, ROWS, BEFORE, cpu.name);
      const s = await gsEval(`machine.${cpu.name}.state`);
      runState = typeof s === 'string' ? s : null;
    } finally {
      loading = false;
    }
  }

  function toggle(): void {
    debug.auxOpen[cpu.name] = !open;
  }
</script>

<CollapsibleSection {title} {open} onToggle={toggle}>
  {#if machine.status === 'running'}
    <p class="aux-hint">Pause the machine to inspect the {cpu.name.toUpperCase()}.</p>
  {:else if !frame}
    <p class="aux-hint">{loading ? 'Reading…' : 'Not available.'}</p>
  {:else}
    <p class="aux-state">
      <span class="aux-label">State</span>
      <span class="aux-value">{runState ?? '—'}</span>
      <span class="aux-label">PC</span>
      <span class="aux-value mono">${fmtHex32(frame.pc)}</span>
    </p>
    {#each groups as group (group.title)}
      <div class="aux-group">
        <h4 class="aux-group-title">{group.title}</h4>
        <div class="aux-regs">
          {#each group.names as name (name)}
            <span class="aux-reg">
              <span class="aux-reg-name">{name.toUpperCase()}</span>
              <span class="aux-reg-value mono"
                >{fmtRegister(frame.arch, name, frame.rawRegs[name])}</span
              >
            </span>
          {/each}
        </div>
      </div>
    {/each}
    {#if frame.fpu && frame.fpu.data.length}
      <div class="aux-group">
        <h4 class="aux-group-title">Floating point</h4>
        {#each frame.fpu.data as reg, i (i)}
          <div class="aux-fp mono">
            <span class="aux-reg-name">{frame.fpu.prefix}{i}</span>
            <span class="aux-fp-val">{reg.val}</span>
            <span class="aux-fp-hex">{reg.hex}</span>
          </div>
        {/each}
      </div>
    {/if}
    <div class="aux-group">
      <h4 class="aux-group-title">Disassembly</h4>
      <ol class="aux-rows mono" aria-label={`${cpu.name} disassembly`}>
        {#each frame.rows as row (row.addr)}
          <li class="aux-row" class:pc={row.addr === frame.pc}>
            <span class="aux-row-addr">{fmtHex32(row.addr)}</span>
            <span class="aux-row-text">{row.mnem}{row.ops ? ` ${row.ops}` : ''}</span>
          </li>
        {/each}
      </ol>
    </div>
  {/if}
</CollapsibleSection>

<style>
  .aux-hint {
    color: var(--gs-fg-muted);
    font-size: 11px;
    padding: 8px 16px;
  }
  .mono {
    font-family: var(--gs-font-mono, ui-monospace, Menlo, monospace);
  }
  .aux-state {
    display: flex;
    gap: 8px;
    align-items: baseline;
    font-size: 11px;
    padding: 6px 12px 0;
    margin: 0;
  }
  .aux-label {
    color: var(--gs-fg-muted);
  }
  .aux-value {
    margin-right: 12px;
  }
  .aux-group {
    padding: 6px 12px;
  }
  .aux-group-title {
    font-size: 10px;
    font-weight: 600;
    color: var(--gs-fg-muted);
    margin: 6px 0 4px;
    text-transform: uppercase;
    letter-spacing: 0.04em;
  }
  .aux-regs {
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(16ch, 1fr));
    column-gap: 16px;
    font-size: 11px;
  }
  .aux-reg {
    display: inline-flex;
    gap: 8px;
  }
  .aux-reg-name {
    color: var(--gs-fg-muted);
    width: 4.5ch;
    text-align: right;
    flex-shrink: 0;
    font-family: var(--gs-font-mono, ui-monospace, Menlo, monospace);
  }
  .aux-fp {
    display: flex;
    gap: 8px;
    font-size: 11px;
  }
  .aux-fp-val {
    min-width: 14ch;
  }
  .aux-fp-hex {
    color: var(--gs-fg-muted);
  }
  .aux-rows {
    list-style: none;
    margin: 0;
    padding: 0;
    font-size: 11px;
  }
  .aux-row {
    display: flex;
    gap: 12px;
    padding: 0 4px;
  }
  .aux-row.pc {
    background: var(--gs-changed-bg);
  }
  .aux-row-addr {
    color: var(--gs-fg-muted);
  }
</style>
