<script lang="ts">
  import { onDestroy } from 'svelte';
  import Hint from '@/components/ui/Hint.svelte';
  import CollapsibleSection from '@/components/common/CollapsibleSection.svelte';
  import Tabs from '@/components/ui/Tabs.svelte';
  import {
    debug,
    toggleSection,
    type LisaContextChoice,
    type MmuSubtab,
  } from '@/state/debug.svelte';
  import { machine } from '@/state/machine.svelte';
  import { debugFrame } from '@/state/debugFrame.svelte';
  import { readMmuState, type MmuRegister } from '@/bus/mmu';
  import MmuStateTab from './MmuStateTab.svelte';
  import MmuTranslateTab from './MmuTranslateTab.svelte';
  import MmuMapTab from './MmuMapTab.svelte';
  import MmuDescriptorsTab from './MmuDescriptorsTab.svelte';
  import SegmentedControl from '@/components/ui/SegmentedControl.svelte';

  // Every tab reads the core's own MMU (bus/mmu.ts): its registers, a
  // walk of one address, the mapped ranges, and raw descriptors.
  const TABS = [
    { key: 'state', label: 'State' },
    { key: 'translate', label: 'Translate' },
    { key: 'map', label: 'Map' },
    { key: 'descriptors', label: 'Descriptors' },
  ] as const;

  // Every MMU kind: the 68030 PMMU, the 68040, the PowerPC 601/604 and the
  // Lisa's segment MMU all answer the same translate / walk / map /
  // descriptor / peek (the section used to show only on a 68030, with
  // fixtures).
  const visible = $derived(machine.mmuKind !== 'none');
  const isLisa = $derived(machine.mmuKind === 'lisa_segment');
  const running = $derived(machine.status === 'running');

  // The MMU's registers, read here for the State tab and for the Lisa's
  // context labels: with each new debug frame while paused, and a few
  // times a second while the machine runs, so a context switch can be
  // watched live (the other tabs walk tables and wait for a pause).
  const LIVE_MS = 250;
  let regs = $state<MmuRegister[]>([]);
  let loaded = $state(false);
  let reading = false;
  async function readRegs(): Promise<void> {
    if (reading) return; // one read in flight at a time
    reading = true;
    try {
      regs = await readMmuState(machine.mmuKind);
      loaded = true;
    } finally {
      reading = false;
    }
  }
  $effect(() => {
    void debugFrame.current;
    void machine.status;
    if (visible && debug.sections.mmu) void readRegs();
  });
  let timer: ReturnType<typeof setInterval> | null = null;
  $effect(() => {
    const live = visible && debug.sections.mmu && running && debug.mmuSubtab === 'state';
    if (live && !timer) {
      timer = setInterval(() => {
        if (typeof document !== 'undefined' && document.visibilityState === 'hidden') return;
        void readRegs();
      }, LIVE_MS);
    } else if (!live && timer) {
      clearInterval(timer);
      timer = null;
    }
  });
  onDestroy(() => {
    if (timer) clearInterval(timer);
  });

  // The Lisa's context the SEG1/SEG2 latches select (user programs run in
  // it; supervisor code always uses context 0).
  const latch = $derived.by(() => {
    const r = regs.find((x) => x.name === 'context');
    return r === undefined ? null : r.value;
  });
  const contextOptions = $derived<{ value: LisaContextChoice; label: string; title: string }[]>([
    {
      value: 'current',
      label: 'Now',
      title: 'Whatever the CPU uses now: context 0 in supervisor mode, else the latch context',
    },
    ...[0, 1, 2, 3].map((c) => ({
      value: String(c) as LisaContextChoice,
      label: `${c}${c === 0 ? ' OS' : ''}${c === latch ? ' •' : ''}`,
      title:
        (c === 0 ? 'Context 0: the operating system' : `Context ${c}: a user program`) +
        (c === latch ? ' (the latches select it now)' : ''),
    })),
  ]);
</script>

{#if visible}
  <CollapsibleSection title="MMU" open={debug.sections.mmu} onToggle={() => toggleSection('mmu')}>
    <Tabs
      tabClass="tab"
      tabs={TABS}
      active={debug.mmuSubtab}
      onSelect={(k: MmuSubtab) => (debug.mmuSubtab = k)}
    >
      {#snippet accessory()}
        {#if isLisa}
          <SegmentedControl
            class="ctx-toggle"
            optionClass="ctx-btn"
            framed
            size="sm"
            label="MMU context"
            value={debug.mmuContext}
            onChange={(v) => (debug.mmuContext = v)}
            options={contextOptions}
          />
        {:else}
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
        {/if}
      {/snippet}
    </Tabs>
    {#if debug.mmuSubtab === 'state'}
      <MmuStateTab {regs} {loaded} live={running} />
    {:else if running}
      <Hint class="mmu-hint" inset="block"
        >Pause the machine to translate, map or read descriptors; the State tab updates while it
        runs.</Hint
      >
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
</style>
