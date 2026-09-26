<script lang="ts">
  import CollapsibleSection from '@/components/common/CollapsibleSection.svelte';
  import { openContextMenu, type ContextMenuItem } from '@/components/common/ContextMenu.svelte';
  import { writeRegister } from '@/bus/debug';
  import { machine } from '@/state/machine.svelte';
  import { showNotification } from '@/state/toasts.svelte';
  import { debug, toggleSection, inspectMemoryAt, bumpDebugRefresh } from '@/state/debug.svelte';
  import { debugFrame } from '@/state/debugFrame.svelte';
  import { fmtHex32, parseHex } from '@/lib/hex';
  import { registerGroups, registerBits, fmtRegister } from '@/lib/registerLayout';

  // Registers shown but not editable from here.
  const READ_ONLY = new Set(['sr']);

  const frame = $derived(debugFrame.current);
  const values = $derived(frame?.rawRegs ?? null);
  const arch = $derived(frame?.arch ?? 'm68k');
  const groups = $derived(frame && values ? registerGroups(frame.arch, values) : []);

  // Highlight the registers that changed since the previous frame; the
  // highlight lasts until the next frame, so single-stepping reads well.
  let prev: Record<string, number> | null = null;
  let changed = $state<Record<string, boolean>>({});
  $effect(() => {
    const next = values;
    if (!next) {
      prev = null;
      changed = {};
      return;
    }
    const flashed: Record<string, boolean> = {};
    if (prev) for (const k of Object.keys(next)) if (prev[k] !== next[k]) flashed[k] = true;
    changed = flashed;
    prev = { ...next };
    debug.registersPrev = prev;
  });

  async function commit(name: string, raw: string, ev: Event) {
    const target = ev.target as HTMLInputElement;
    const value = parseHex(raw);
    // Only a register this frame reported can be written: the name becomes
    // a path segment of machine.cpu.
    if (value === null || !values || !(name in values)) {
      target.classList.add('invalid');
      setTimeout(() => target.classList.remove('invalid'), 400);
      showNotification(`Invalid hex value for ${name.toUpperCase()}`, 'error');
      return;
    }
    const ok = await writeRegister(name, value);
    if (!ok) showNotification(`Failed to write ${name.toUpperCase()}`, 'error');
    bumpDebugRefresh();
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
    return fmtRegister(arch, name, values?.[name] ?? 0);
  }

  function widthChFor(name: string): number {
    return registerBits(arch, name) / 4;
  }

  function onRegContext(name: string, ev: MouseEvent) {
    if (READ_ONLY.has(name)) return; // no context menu on status registers
    ev.preventDefault();
    const value = values?.[name] ?? 0;
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
</script>

<CollapsibleSection
  title="Registers"
  open={debug.sections.registers}
  onToggle={() => toggleSection('registers')}
>
  {#if machine.status === 'running'}
    <p class="reg-hint">Pause the machine to inspect register state.</p>
  {:else if !frame}
    <p class="reg-hint">{debugFrame.loading ? 'Reading registers…' : 'No machine running.'}</p>
  {:else}
    {#each groups as group (group.title)}
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
                readonly={READ_ONLY.has(name)}
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
