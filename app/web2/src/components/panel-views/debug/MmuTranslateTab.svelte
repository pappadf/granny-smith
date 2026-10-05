<script lang="ts">
  import Badge from '@/components/ui/Badge.svelte';
  import { walkAddr, type Walk } from '@/bus/mmu';
  import { formatEntry } from '@/lib/mmu';
  import { debug } from '@/state/debug.svelte';
  import { debugFrame } from '@/state/debugFrame.svelte';
  import { fmtHex32, parseHex } from '@/lib/hex';
  import Button from '@/components/ui/Button.svelte';
  import TextInput from '@/components/ui/TextInput.svelte';

  // inputValue mirrors debug.mmuTransAddr but is mutable so the user can
  // type into it before committing with Enter / Translate.
  // eslint-disable-next-line svelte/prefer-writable-derived
  let inputValue = $state(fmtHex32(debug.mmuTransAddr));
  let result = $state<Walk | null>(null);

  $effect(() => {
    inputValue = fmtHex32(debug.mmuTransAddr);
  });

  // Walk through the core whenever the address, the S/U choice or the
  // machine state (a new frame) changes: the translation, and every
  // register, table level or page-table group the MMU consulted for it.
  $effect(() => {
    void debugFrame.current;
    const addr = debug.mmuTransAddr;
    const sup = debug.mmuSupervisor;
    void walkAddr(addr, sup).then((w) => (result = w));
  });

  function translate() {
    const v = parseHex(inputValue);
    if (v === null) return;
    debug.mmuTransAddr = v;
  }

  function onKey(ev: KeyboardEvent) {
    if (ev.key === 'Enter') {
      ev.preventDefault();
      translate();
    }
  }
</script>

<div class="trans-body">
  <div class="trans-header">
    <span class="lbl">Address:</span>
    <TextInput
      class="addr"
      hex
      widthCh={10}
      bind:value={inputValue}
      onkeydown={onKey}
      aria-label="Logical address to translate"
    />
    <Button class="btn" onclick={translate}>Translate</Button>
    {#if debugFrame.current}
      <span class="presets">
        <Button
          class="preset-btn"
          onclick={() => (debug.mmuTransAddr = debugFrame.current?.pc ?? 0)}>PC</Button
        >
      </span>
    {/if}
  </div>
  {#if result}
    {#if !result.valid}
      <p class="invalid">
        L:$<span class="hex">{fmtHex32(debug.mmuTransAddr)}</span> → INVALID — faults if accessed
      </p>
    {:else}
      <p class="ok">
        L:$<span class="hex">{fmtHex32(debug.mmuTransAddr)}</span> P:$<span class="hex"
          >{fmtHex32(result.phys ?? 0)}</span
        >
        <Badge class="tag tag-pt" intent="info">{result.via.toUpperCase()}</Badge>
        {#if result.access}<Badge class="tag tag-acc" intent="neutral">{result.access}</Badge>{/if}
        {#if result.space}<Badge class="tag tag-tt" intent="success">{result.space}</Badge>{/if}
      </p>
    {/if}
    {#if result.steps.length === 0}
      <p class="no-steps">Translation is off: every address is its own physical address.</p>
    {/if}
    <ol class="steps">
      {#each result.steps as s, i (i)}
        {@const e = formatEntry(s.step, s.fields)}
        <li class="step" data-outcome={s.outcome}>
          <span class="step-title">{e.title}</span>
          <span class="step-outcome">{s.outcome}</span>
          <span class="step-main">{e.main}</span>
          {#if e.extra.length}<span class="step-extra">{e.extra.join(' · ')}</span>{/if}
        </li>
      {/each}
    </ol>
  {/if}
</div>

<style>
  .trans-body {
    padding: var(--gs-space-2) var(--gs-space-3);
    display: flex;
    flex-direction: column;
    gap: var(--gs-space-1-5);
  }
  .trans-header {
    display: flex;
    align-items: center;
    gap: var(--gs-space-1-5);
    flex-wrap: wrap;
  }
  .lbl {
    color: var(--gs-text-muted);
    font-size: var(--gs-font-size-xs);
  }
  .presets {
    display: inline-flex;
    gap: var(--gs-space-1);
    margin-left: var(--gs-space-3);
  }
  .invalid {
    color: var(--gs-danger-fg);
    font-family: var(--gs-font-mono);
    font-size: var(--gs-font-size-xs);
    margin: var(--gs-space-1-5) 0 0;
  }
  .ok {
    color: var(--gs-text);
    font-family: var(--gs-font-mono);
    font-size: var(--gs-font-size-xs);
    margin: var(--gs-space-1-5) 0 0;
  }
  .hex {
    text-transform: uppercase; /* hex digits */
  }
  .ok :global(.tag) {
    margin-left: var(--gs-space-1-5);
  }
  .no-steps {
    color: var(--gs-text-muted);
    font-size: var(--gs-font-size-xs);
    margin: 0;
  }
  .steps {
    list-style: none;
    margin: var(--gs-space-1) 0 0;
    padding: 0;
    display: flex;
    flex-direction: column;
    gap: var(--gs-space-1);
  }
  .step {
    display: grid;
    grid-template-columns: 15ch 5ch 1fr;
    column-gap: var(--gs-space-2);
    font-family: var(--gs-font-mono);
    font-size: var(--gs-font-size-xs);
    line-height: 1.5;
    border-left: var(--gs-border-width-strong) solid var(--gs-border);
    padding-left: var(--gs-space-1-5);
  }
  .step[data-outcome='hit'] {
    border-left-color: var(--gs-success-fg);
  }
  .step[data-outcome='fault'] {
    border-left-color: var(--gs-danger-fg);
  }
  .step-title {
    color: var(--gs-text);
  }
  .step-outcome {
    color: var(--gs-text-muted);
  }
  .step-main {
    color: var(--gs-text);
    overflow-wrap: anywhere;
  }
  .step-extra {
    grid-column: 3;
    color: var(--gs-text-muted);
    overflow-wrap: anywhere;
  }
</style>
