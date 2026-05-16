<script lang="ts">
  import CollapsibleSection from '@/components/common/CollapsibleSection.svelte';
  import TabStrip from '@/components/common/TabStrip.svelte';
  import { debug, toggleSection, type MmuSubtab } from '@/state/debug.svelte';
  import { machine } from '@/state/machine.svelte';
  import MmuStateTab from './MmuStateTab.svelte';
  import MmuTranslateTab from './MmuTranslateTab.svelte';
  import MmuMapTab from './MmuMapTab.svelte';
  import MmuDescriptorsTab from './MmuDescriptorsTab.svelte';

  const TABS = [
    { key: 'state', label: 'State' },
    { key: 'translate', label: 'Translate' },
    { key: 'map', label: 'Map' },
    { key: 'descriptors', label: 'Descriptors' },
  ] as const;

  const visible = $derived(machine.mmuEnabled);
</script>

{#if visible}
  <CollapsibleSection title="MMU" open={debug.sections.mmu} onToggle={() => toggleSection('mmu')}>
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
            onclick={() => (debug.mmuSupervisor = true)}
            title="Supervisor root"
          >
            S
          </button>
          <button
            type="button"
            class="su-btn"
            class:active={!debug.mmuSupervisor}
            onclick={() => (debug.mmuSupervisor = false)}
            title="User root"
          >
            U
          </button>
        </div>
      {/snippet}
    </TabStrip>
    {#if debug.mmuSubtab === 'state'}
      <MmuStateTab />
    {:else if debug.mmuSubtab === 'translate'}
      <MmuTranslateTab />
    {:else if debug.mmuSubtab === 'map'}
      <MmuMapTab />
    {:else}
      <MmuDescriptorsTab />
    {/if}
  </CollapsibleSection>
{/if}

<style>
  .su-toggle {
    display: inline-flex;
    border: 1px solid var(--gs-border);
    border-radius: 2px;
    overflow: hidden;
    height: 20px;
  }
  .su-btn {
    background: transparent;
    color: var(--gs-fg-muted);
    border: none;
    padding: 0 8px;
    font-size: 11px;
    font-weight: 600;
    cursor: pointer;
  }
  .su-btn.active {
    background: var(--gs-row-selected, rgba(80, 140, 220, 0.25));
    color: var(--gs-fg-bright);
  }
</style>
