<script lang="ts">
  import CollapsibleSection from '@/components/common/CollapsibleSection.svelte';
  import TabStrip from '@/components/common/TabStrip.svelte';
  import { debug, toggleSection, type MmuSubtab } from '@/state/debug.svelte';
  import { machine } from '@/state/machine.svelte';
  import MmuStateTab from './MmuStateTab.svelte';
  import MmuTranslateTab from './MmuTranslateTab.svelte';

  // State and Translate read the core's own MMU (bus/mmu.ts).  Map and
  // Descriptors are gone until the core can walk a table for them: they
  // showed a hand-written SE/30 layout as if it were live.
  const TABS = [
    { key: 'state', label: 'State' },
    { key: 'translate', label: 'Translate' },
  ] as const;

  // Every MMU kind: the 68030 PMMU, the 68040, the PowerPC 601/604 and the
  // Lisa's segment MMU all answer the same translate/peek (the section used
  // to show only on a 68030, with fixtures).
  const visible = $derived(machine.mmuKind !== 'none');
</script>

{#if visible}
  <CollapsibleSection title="MMU" open={debug.sections.mmu} onToggle={() => toggleSection('mmu')}>
    {#if machine.status === 'running'}
      <p class="mmu-hint">Pause the machine to inspect MMU state.</p>
    {:else}
      <TabStrip
        tabs={TABS}
        active={debug.mmuSubtab}
        onSelect={(k: MmuSubtab) => (debug.mmuSubtab = k)}
      >
        {#snippet accessory()}
          <div class="su-toggle" role="group" aria-label="Supervisor / User root">
            <button
              type="button"
              class="su-btn"
              class:active={debug.mmuSupervisor}
              aria-pressed={debug.mmuSupervisor}
              onclick={() => (debug.mmuSupervisor = true)}
              title="Supervisor root"
            >
              S
            </button>
            <button
              type="button"
              class="su-btn"
              class:active={!debug.mmuSupervisor}
              aria-pressed={!debug.mmuSupervisor}
              onclick={() => (debug.mmuSupervisor = false)}
              title="User root"
            >
              U
            </button>
          </div>
        {/snippet}
      </TabStrip>
      {#if debug.mmuSubtab === 'translate'}
        <MmuTranslateTab />
      {:else}
        <MmuStateTab />
      {/if}
    {/if}
  </CollapsibleSection>
{/if}

<style>
  .mmu-hint {
    color: var(--gs-text-muted);
    font-size: var(--gs-font-size-xs);
    padding: var(--gs-space-2) var(--gs-space-4);
  }
  .su-toggle {
    display: inline-flex;
    border: var(--gs-border-width) solid var(--gs-border);
    border-radius: var(--gs-radius-xs);
    overflow: hidden;
    height: 20px;
  }
  .su-btn {
    background: transparent;
    color: var(--gs-text-muted);
    border: none;
    padding: 0 var(--gs-space-2);
    font-size: var(--gs-font-size-xs);
    font-weight: var(--gs-font-weight-semibold);
    cursor: pointer;
  }
  .su-btn.active {
    background: var(--gs-row-selected);
    color: var(--gs-text-strong);
  }
</style>
