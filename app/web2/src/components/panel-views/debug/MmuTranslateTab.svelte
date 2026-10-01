<script lang="ts">
  import { translateAddr, type Translation } from '@/bus/mmu';
  import { debug } from '@/state/debug.svelte';
  import { debugFrame } from '@/state/debugFrame.svelte';
  import { fmtHex32, parseHex } from '@/lib/hex';
  import Button from '@/components/ui/Button.svelte';
  import TextInput from '@/components/ui/TextInput.svelte';

  // inputValue mirrors debug.mmuTransAddr but is mutable so the user can
  // type into it before committing with Enter / Translate.
  // eslint-disable-next-line svelte/prefer-writable-derived
  let inputValue = $state(fmtHex32(debug.mmuTransAddr));
  let result = $state<Translation | null>(null);

  $effect(() => {
    inputValue = fmtHex32(debug.mmuTransAddr);
  });

  // Translate through the core whenever the address, the S/U choice or the
  // machine state (a new frame) changes.
  $effect(() => {
    void debugFrame.current;
    const addr = debug.mmuTransAddr;
    const sup = debug.mmuSupervisor;
    void translateAddr(addr, sup).then((t) => (result = t));
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
        <span class="tag tag-pt">{result.via.toUpperCase()}</span>
        {#if result.space}<span class="tag tag-tt">{result.space}</span>{/if}
      </p>
    {/if}
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
  .tag {
    border-radius: var(--gs-radius-pill);
    padding: 0 var(--gs-space-1-5);
    font-size: var(--gs-font-size-2xs);
    font-weight: var(--gs-font-weight-semibold);
    margin-left: var(--gs-space-1-5);
    text-transform: var(--gs-caps-transform);
  }
  .tag-tt {
    background: var(--gs-success-bg);
    color: var(--gs-success-fg);
  }
  .tag-pt {
    background: var(--gs-info-bg);
    color: var(--gs-info-fg);
  }
</style>
