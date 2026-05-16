<script lang="ts">
  import CollapsibleSection from '@/components/common/CollapsibleSection.svelte';
  import { readRegisters, peekL } from '@/bus/debug';
  import { machine } from '@/state/machine.svelte';
  import { debug, toggleSection } from '@/state/debug.svelte';
  import { mmuLookup } from '@/bus/mockMmu';
  import { fmtHex32 } from '@/lib/hex';

  interface Frame {
    ret: number;
    frame: number;
  }

  let frames = $state<Frame[]>([]);
  let loading = $state(false);

  const MAX_DEPTH = 16;

  async function refresh() {
    loading = true;
    try {
      const regs = await readRegisters();
      if (!regs) {
        frames = [];
        return;
      }
      const result: Frame[] = [];
      let frame = regs.a[6] >>> 0;
      // Plain object as a visited-marker — avoids the prefer-svelte-reactivity
      // lint for plain JS Set; this is local-only, never reactive.
      const seen: Record<number, true> = {};
      while (frame && result.length < MAX_DEPTH && !seen[frame]) {
        seen[frame] = true;
        const ret = await peekL(frame + 4);
        if (ret === null) break;
        const next = await peekL(frame);
        result.push({ ret: ret >>> 0, frame });
        if (next === null) break;
        frame = next >>> 0;
      }
      frames = result;
    } finally {
      loading = false;
    }
  }

  $effect(() => {
    void machine.status;
    if (debug.sections.callstack) void refresh();
  });

  function labelFor(addr: number): string {
    if (!machine.mmuEnabled) return `$${fmtHex32(addr)}`;
    const r = mmuLookup(addr);
    const phys = r.valid && r.phys !== undefined ? fmtHex32(r.phys) : '!';
    const tag = r.valid ? (r.kind ?? 'PT') : 'INVALID';
    return `L:$${fmtHex32(addr)}  P:$${phys}  ${tag}`;
  }
</script>

<CollapsibleSection
  title="Call Stack"
  open={debug.sections.callstack}
  onToggle={() => toggleSection('callstack')}
>
  {#if loading && frames.length === 0}
    <p class="hint">Reading frames…</p>
  {:else if frames.length === 0}
    <p class="hint">No stack frames available.</p>
  {:else}
    {#each frames as f, i (i)}
      <div class="frame-row">
        <span class="idx">#{i}</span>
        <span class="ret">{labelFor(f.ret)}</span>
        <span class="frame">fp=${fmtHex32(f.frame)}</span>
      </div>
    {/each}
  {/if}
</CollapsibleSection>

<style>
  .hint {
    color: var(--gs-fg-muted);
    font-size: 12px;
    padding: 6px 12px;
  }
  .frame-row {
    display: flex;
    align-items: center;
    gap: 12px;
    padding: 2px 12px;
    font-family: var(--gs-font-mono, ui-monospace, Menlo, monospace);
    font-size: 12px;
    color: var(--gs-fg);
  }
  .idx {
    color: var(--gs-fg-muted);
    width: 3ch;
  }
  .frame {
    color: var(--gs-fg-muted);
  }
</style>
