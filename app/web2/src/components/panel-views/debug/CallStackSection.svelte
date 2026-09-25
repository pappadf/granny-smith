<script lang="ts">
  import CollapsibleSection from '@/components/common/CollapsibleSection.svelte';
  import { peekLogicalL } from '@/bus/debug';
  import { machine } from '@/state/machine.svelte';
  import { debug, toggleSection } from '@/state/debug.svelte';
  import { debugFrame } from '@/state/debugFrame.svelte';
  import { mmuLookup } from '@/bus/mockMmu';
  import { fmtHex32 } from '@/lib/hex';

  interface Frame {
    ret: number;
    frame: number;
  }

  let frames = $state<Frame[]>([]);
  let loading = $state(false);

  const MAX_DEPTH = 16;

  // Walk the stack from the shared frame's registers — no second register
  // read (this pane used to issue 20 separate ones).  Per architecture:
  //   68K: the A6 link chain — [fp] is the caller's fp, [fp+4] the return.
  //   PPC: the r1 back-chain — [sp] is the caller's sp, and the saved LR
  //        sits at 8 bytes into the caller's frame; LR itself is frame #0.
  // Reads are of LOGICAL addresses on both (peekLogicalL).
  async function walk(): Promise<Frame[]> {
    const f = debugFrame.current;
    if (!f) return [];
    const out: Frame[] = [];
    const seen: Record<number, true> = {}; // loop guard (local, never reactive)
    if (f.arch === 'ppc') {
      out.push({ ret: f.rawRegs.lr ?? 0, frame: f.rawRegs.r1 ?? 0 });
      let sp = (f.rawRegs.r1 ?? 0) >>> 0;
      while (sp && out.length < MAX_DEPTH && !seen[sp]) {
        seen[sp] = true;
        const caller = await peekLogicalL(sp, 'ppc');
        if (!caller) break;
        const ret = await peekLogicalL(caller + 8, 'ppc');
        if (ret === null) break;
        out.push({ ret: ret >>> 0, frame: caller >>> 0 });
        sp = caller >>> 0;
      }
      return out;
    }
    let fp = (f.rawRegs.a6 ?? 0) >>> 0;
    while (fp && out.length < MAX_DEPTH && !seen[fp]) {
      seen[fp] = true;
      const ret = await peekLogicalL(fp + 4, f.arch);
      if (ret === null) break;
      const next = await peekLogicalL(fp, f.arch);
      out.push({ ret: ret >>> 0, frame: fp });
      if (next === null) break;
      fp = next >>> 0;
    }
    return out;
  }

  // Re-walk when a new frame arrives and the section is open.
  $effect(() => {
    void debugFrame.current;
    if (!debug.sections.callstack) return;
    loading = true;
    void walk()
      .then((r) => (frames = r))
      .finally(() => (loading = false));
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
  {#if machine.status === 'running'}
    <p class="hint">Pause the machine to inspect the call stack.</p>
  {:else if loading && frames.length === 0}
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
    font-size: 11px;
    padding: 6px 12px;
  }
  .frame-row {
    display: flex;
    align-items: center;
    gap: 12px;
    padding: 2px 12px;
    font-family: var(--gs-font-mono, ui-monospace, Menlo, monospace);
    font-size: 11px;
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
