<script lang="ts">
  import SectionHeading from '@/components/ui/SectionHeading.svelte';
  import Hint from '@/components/ui/Hint.svelte';
  import CollapsibleSection from '@/components/common/CollapsibleSection.svelte';
  import { openContextMenu, type ContextMenuItem } from '@/components/common/ContextMenu.svelte';
  import { writeRegister } from '@/bus/debug';
  import { machine } from '@/state/machine.svelte';
  import { showNotification } from '@/state/toasts.svelte';
  import { debug, toggleSection, inspectMemoryAt, bumpDebugRefresh } from '@/state/debug.svelte';
  import { debugFrame } from '@/state/debugFrame.svelte';
  import { fmtHex32, parseHex } from '@/lib/hex';
  import { registerGroups, registerBits, fmtRegister } from '@/lib/registerLayout';
  import InlineValueInput from '@/components/ui/InlineValueInput.svelte';

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

  // A refused entry flashes its field for 400 ms.
  let invalid = $state<Record<string, boolean>>({});
  function flashInvalid(name: string): void {
    invalid = { ...invalid, [name]: true };
    setTimeout(() => (invalid = { ...invalid, [name]: false }), 400);
  }

  async function commit(name: string, raw: string) {
    const value = parseHex(raw);
    // Only a register this frame reported can be written: the name becomes
    // a path segment of machine.cpu.
    if (value === null || !values || !(name in values)) {
      flashInvalid(name);
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
      void commit(name, target.value);
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
    <Hint class="reg-hint" inset="block">Pause the machine to inspect register state.</Hint>
  {:else if !frame}
    <Hint class="reg-hint" inset="block"
      >{debugFrame.loading ? 'Reading registers…' : 'No machine running.'}</Hint
    >
  {:else}
    {#each groups as group (group.title)}
      <div class="reg-group">
        <SectionHeading level="h4" class="reg-group-title">{group.title}</SectionHeading>
        <div
          class="reg-rows"
          style="grid-template-rows: repeat({Math.ceil(group.names.length / 2)}, auto);"
        >
          {#each group.names as name (name)}
            <!-- svelte-ignore a11y_no_static_element_interactions -->
            <div class="reg-row" oncontextmenu={(ev) => onRegContext(name, ev)}>
              <span class="reg-name">{name.toUpperCase()}</span>
              <InlineValueInput
                class="reg-value"
                changed={changed[name]}
                invalid={invalid[name]}
                value={currentValueFor(name)}
                size={widthChFor(name)}
                widthCh={widthChFor(name)}
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
  .reg-group {
    padding: var(--gs-space-1-5) var(--gs-space-3);
  }
  .reg-rows {
    /* `auto auto` keeps both columns content-width so the right-hand
       registers sit next to the left ones rather than drifting to the
       far edge of a wide section. */
    display: grid;
    grid-auto-flow: column;
    grid-template-columns: auto auto;
    column-gap: var(--gs-space-6);
    row-gap: 0;
    justify-content: start;
  }
  .reg-row {
    display: inline-flex;
    align-items: center;
    gap: var(--gs-space-2);
    font-family: var(--gs-font-mono);
    font-size: var(--gs-font-size-xs);
  }
  .reg-name {
    color: var(--gs-text-muted);
    width: 3.5ch;
    text-align: right;
    flex-shrink: 0;
  }
</style>
