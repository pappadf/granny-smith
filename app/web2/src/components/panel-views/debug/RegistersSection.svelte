<script lang="ts">
  import { onMount } from 'svelte';
  import CollapsibleSection from '@/components/common/CollapsibleSection.svelte';
  import { openContextMenu, type ContextMenuItem } from '@/components/common/ContextMenu.svelte';
  import { loadDebugFrame, writeRegister, type Registers } from '@/bus/debug';
  import { machine } from '@/state/machine.svelte';
  import { showNotification } from '@/state/toasts.svelte';
  import { debug, toggleSection, inspectMemoryAt } from '@/state/debug.svelte';
  import { fmtHex32, fmtHex16, parseHex } from '@/lib/hex';

  let regs = $state<Registers | null>(null);
  // Highlight set: register names that just changed.
  let changed = $state<Record<string, boolean>>({});

  async function refresh() {
    // Reuse the bundled debug.frame call (one bridge round-trip for
    // registers + disasm + MMU). DisassemblyPane fires the same call
    // independently — total per-pause cost is 2 bridge calls, down
    // from ~40 (20 here, 20+1 there). Sharing a single fetch is a
    // future polish; the duplicate response is small (~5 KB) and the
    // C-side cost is sub-millisecond.
    const frame = await loadDebugFrame();
    if (!frame) {
      regs = null;
      return;
    }
    const next = frame.regs;
    if (regs) {
      // Highlight registers that just changed and KEEP the highlight
      // until the next refresh — `changed` is fully replaced below,
      // so the next step naturally clears the previous flash and
      // marks only the newly-changed registers. No auto-decay timer,
      // so single-stepping feels instant.
      const prevMap = flattenRegs(regs);
      const nextMap = flattenRegs(next);
      const flashed: Record<string, boolean> = {};
      for (const k of Object.keys(nextMap)) {
        if (prevMap[k] !== nextMap[k]) flashed[k] = true;
      }
      changed = flashed;
    }
    regs = next;
    debug.registersPrev = flattenRegs(next);
  }

  function flattenRegs(r: Registers): Record<string, number> {
    const out: Record<string, number> = {};
    for (let i = 0; i < 8; i++) out[`d${i}`] = r.d[i] ?? 0;
    for (let i = 0; i < 8; i++) out[`a${i}`] = r.a[i] ?? 0;
    out.pc = r.pc;
    out.sr = r.sr;
    out.usp = r.usp;
    out.ssp = r.ssp;
    return out;
  }

  onMount(() => {
    void refresh();
  });

  // Re-fetch when the machine pauses (post-step / breakpoint hit).
  // Also watches `debug.refreshGen` — Steps don't change run-state
  // (paused → paused) so we need an explicit "PC moved" signal to
  // trigger a re-fetch.
  $effect(() => {
    void machine.status;
    void debug.refreshGen;
    if (machine.status === 'paused' || machine.status === 'running') {
      void refresh();
    }
  });

  async function commit(name: string, raw: string, ev: Event) {
    const target = ev.target as HTMLInputElement;
    const value = parseHex(raw);
    if (value === null) {
      target.classList.add('invalid');
      setTimeout(() => target.classList.remove('invalid'), 400);
      showNotification(`Invalid hex value for ${name.toUpperCase()}`, 'error');
      return;
    }
    const ok = await writeRegister(name, value);
    if (!ok) showNotification(`Failed to write ${name.toUpperCase()}`, 'error');
    await refresh();
  }

  function onKey(name: string, ev: KeyboardEvent) {
    const target = ev.target as HTMLInputElement;
    if (ev.key === 'Enter') {
      ev.preventDefault();
      void commit(name, target.value, ev);
    } else if (ev.key === 'Escape') {
      ev.preventDefault();
      target.value = currentValueFor(name);
      target.blur();
    }
  }

  function currentValueFor(name: string): string {
    if (!regs) return '00000000';
    if (name === 'sr') return fmtHex16(regs.sr);
    const flat = flattenRegs(regs);
    return fmtHex32(flat[name] ?? 0);
  }

  function widthChFor(name: string): number {
    return name === 'sr' ? 4 : 8;
  }

  function onRegContext(name: string, ev: MouseEvent) {
    if (name === 'sr') return; // SR is non-editable + no context menu
    ev.preventDefault();
    const flat = regs ? flattenRegs(regs) : {};
    const value = flat[name] ?? 0;
    const valueText = `$${fmtHex32(value)}`;
    const upper = name.toUpperCase();
    const items: ContextMenuItem[] = [
      {
        label: `Inspect memory at ${upper} (${valueText})`,
        action: () => inspectMemoryAt(value),
      },
      {
        label: 'Copy value',
        action: () => {
          if (navigator.clipboard?.writeText) void navigator.clipboard.writeText(valueText);
        },
      },
    ];
    openContextMenu(items, ev.clientX, ev.clientY);
  }

  // Three groups, each rendered as a 2-column grid.
  const GROUPS = [
    { title: 'Data', names: ['d0', 'd1', 'd2', 'd3', 'd4', 'd5', 'd6', 'd7'] },
    { title: 'Address', names: ['a0', 'a1', 'a2', 'a3', 'a4', 'a5', 'a6', 'a7'] },
    { title: 'Control', names: ['pc', 'sr', 'usp', 'ssp'] },
  ];
</script>

<CollapsibleSection
  title="Registers"
  open={debug.sections.registers}
  onToggle={() => toggleSection('registers')}
>
  {#if machine.status === 'running'}
    <p class="reg-hint">Pause the machine to inspect register state.</p>
  {:else if !regs}
    <p class="reg-hint">No machine running.</p>
  {:else}
    {#each GROUPS as group (group.title)}
      <div class="reg-group">
        <h4 class="reg-group-title">{group.title}</h4>
        <div
          class="reg-rows"
          style="grid-template-rows: repeat({Math.ceil(group.names.length / 2)}, auto);"
        >
          {#each group.names as name (name)}
            <!-- svelte-ignore a11y_no_static_element_interactions -->
            <div class="reg-row" oncontextmenu={(ev) => onRegContext(name, ev)}>
              <span class="reg-name">{name.toUpperCase()}</span>
              <input
                class="reg-value"
                class:changed={changed[name]}
                type="text"
                value={currentValueFor(name)}
                size={widthChFor(name)}
                style="width: {widthChFor(name)}ch;"
                readonly={name === 'sr'}
                aria-label={`${name.toUpperCase()} register value`}
                onkeydown={(ev) => onKey(name, ev)}
              />
            </div>
          {/each}
        </div>
      </div>
    {/each}
  {/if}
</CollapsibleSection>

<style>
  .reg-hint {
    color: var(--gs-fg-muted);
    font-size: 11px;
    padding: 8px 16px;
  }
  .reg-group {
    padding: 6px 12px;
  }
  .reg-group-title {
    font-size: 10px;
    font-weight: 600;
    color: var(--gs-fg-muted);
    margin: 6px 0 4px;
    text-transform: uppercase;
    letter-spacing: 0.04em;
  }
  .reg-rows {
    /* `auto auto` keeps both columns content-width so the right-hand
       registers sit next to the left ones rather than drifting to the
       far edge of a wide section. */
    display: grid;
    grid-auto-flow: column;
    grid-template-columns: auto auto;
    column-gap: 24px;
    row-gap: 0;
    justify-content: start;
  }
  .reg-row {
    display: inline-flex;
    align-items: center;
    gap: 8px;
    font-family: var(--gs-font-mono, ui-monospace, Menlo, monospace);
    font-size: 11px;
  }
  .reg-name {
    color: var(--gs-fg-muted);
    width: 3.5ch;
    text-align: right;
    flex-shrink: 0;
  }
  .reg-value {
    /* Reset to content-box so `width: 8ch` (set inline) reflects the
       hex digit content area, not the total box. The global reset
       applies border-box to everything, which truncates the value to
       ~6.5 chars after subtracting 8 px of padding + 2 px of border. */
    box-sizing: content-box;
    background: transparent;
    color: var(--gs-fg);
    border: 1px solid transparent;
    border-radius: 2px;
    padding: 0 4px;
    height: 18px;
    font-family: inherit;
    font-size: inherit;
    outline: none;
    text-transform: uppercase;
  }
  .reg-value:hover {
    border-color: var(--gs-input-border);
  }
  .reg-value:focus {
    border-color: var(--gs-focus, #0969da);
    background: var(--gs-input-bg, rgba(0, 0, 0, 0.2));
  }
  .reg-value.changed {
    background: var(--gs-changed-bg);
  }
  .reg-value:global(.invalid) {
    border-color: var(--gs-error-fg, #f48771) !important;
  }
  .reg-value[readonly] {
    cursor: default;
  }
</style>
