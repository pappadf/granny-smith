<script lang="ts">
  import { onMount } from 'svelte';
  import CollapsibleSection from '@/components/common/CollapsibleSection.svelte';
  import { openContextMenu, type ContextMenuItem } from '@/components/common/ContextMenu.svelte';
  import { readRegisters, writeRegister, type Registers } from '@/bus/debug';
  import { machine } from '@/state/machine.svelte';
  import { showNotification } from '@/state/toasts.svelte';
  import { debug, toggleSection, inspectMemoryAt } from '@/state/debug.svelte';
  import { fmtHex32, fmtHex16, parseHex } from '@/lib/hex';

  let regs = $state<Registers | null>(null);
  // Highlight set: register names that just changed.
  let changed = $state<Record<string, boolean>>({});

  async function refresh() {
    const next = await readRegisters();
    if (!next) {
      regs = null;
      return;
    }
    if (regs) {
      const prevMap = flattenRegs(regs);
      const nextMap = flattenRegs(next);
      const flashed: Record<string, boolean> = {};
      for (const k of Object.keys(nextMap)) {
        if (prevMap[k] !== nextMap[k]) flashed[k] = true;
      }
      changed = flashed;
      setTimeout(() => (changed = {}), 800);
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
  $effect(() => {
    void machine.status;
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
  {#if !regs}
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
    font-size: 12px;
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
    display: grid;
    grid-auto-flow: column;
    grid-template-columns: 1fr 1fr;
    column-gap: 16px;
    row-gap: 2px;
  }
  .reg-row {
    display: inline-flex;
    align-items: center;
    gap: 8px;
    font-family: var(--gs-font-mono, ui-monospace, Menlo, monospace);
    font-size: 13px;
  }
  .reg-name {
    color: var(--gs-fg-muted);
    width: 3.5ch;
    text-align: right;
    flex-shrink: 0;
  }
  .reg-value {
    background: transparent;
    color: var(--gs-fg);
    border: 1px solid transparent;
    border-radius: 2px;
    padding: 1px 4px;
    height: 20px;
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
    background: rgba(255, 220, 64, 0.18);
  }
  .reg-value:global(.invalid) {
    border-color: var(--gs-error-fg, #f48771) !important;
  }
  .reg-value[readonly] {
    cursor: default;
  }
</style>
