<script lang="ts">
  import Hint from '@/components/ui/Hint.svelte';
  import CollapsibleSection from '@/components/common/CollapsibleSection.svelte';
  import Tabs from '@/components/ui/Tabs.svelte';
  import { debug, toggleSection, type MmuSubtab } from '@/state/debug.svelte';
  import { machine } from '@/state/machine.svelte';
  import MmuStateTab from './MmuStateTab.svelte';
  import MmuTranslateTab from './MmuTranslateTab.svelte';
  import SegmentedControl from '@/components/ui/SegmentedControl.svelte';

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
      <Hint class="mmu-hint" inset="block">Pause the machine to inspect MMU state.</Hint>
    {:else}
      <Tabs
        tabClass="tab"
        tabs={TABS}
        active={debug.mmuSubtab}
        onSelect={(k: MmuSubtab) => (debug.mmuSubtab = k)}
      >
        {#snippet accessory()}
          <SegmentedControl
            class="su-toggle"
            optionClass="su-btn"
            framed
            label="Supervisor / User root"
            value={debug.mmuSupervisor}
            onChange={(v) => (debug.mmuSupervisor = v)}
            options={[
              { value: true, label: 'S', title: 'Supervisor root' },
              { value: false, label: 'U', title: 'User root' },
            ]}
          />
        {/snippet}
      </Tabs>
      {#if debug.mmuSubtab === 'translate'}
        <MmuTranslateTab />
      {:else}
        <MmuStateTab />
      {/if}
    {/if}
  </CollapsibleSection>
{/if}

<style>
</style>
