<script lang="ts">
  import { translateAddr, type Translation } from '@/bus/mmu';
  import { debug } from '@/state/debug.svelte';
  import { debugFrame } from '@/state/debugFrame.svelte';
  import { fmtHex32, parseHex } from '@/lib/hex';

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
    <input
      type="text"
      class="addr"
      bind:value={inputValue}
      onkeydown={onKey}
      aria-label="Logical address to translate"
    />
    <button type="button" class="btn" onclick={translate}>Translate</button>
    {#if debugFrame.current}
      <span class="presets">
        <button
          type="button"
          class="preset-btn"
          onclick={() => (debug.mmuTransAddr = debugFrame.current?.pc ?? 0)}>PC</button
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
    padding: 8px 12px;
    display: flex;
    flex-direction: column;
    gap: 6px;
  }
  .trans-header {
    display: flex;
    align-items: center;
    gap: 6px;
    flex-wrap: wrap;
  }
  .lbl {
    color: var(--gs-fg-muted);
    font-size: 11px;
  }
  .addr {
    background: var(--gs-input-bg);
    color: var(--gs-input-fg);
    border: 1px solid var(--gs-input-border);
    border-radius: 2px;
    height: 22px;
    padding: 0 6px;
    font-family: var(--gs-font-mono, ui-monospace, Menlo, monospace);
    font-size: 11px;
    width: 10ch;
    outline: none;
    text-transform: uppercase;
  }
  .addr:focus {
    border-color: var(--gs-focus);
  }
  .btn,
  .preset-btn {
    background: transparent;
    color: var(--gs-fg);
    border: 1px solid var(--gs-border);
    border-radius: 2px;
    height: 22px;
    padding: 0 8px;
    font-size: 11px;
    font-family: var(--gs-font-mono, ui-monospace, Menlo, monospace);
    cursor: pointer;
  }
  .btn:hover,
  .preset-btn:hover {
    background: var(--gs-row-hover, rgba(255, 255, 255, 0.06));
  }
  .presets {
    display: inline-flex;
    gap: 4px;
    margin-left: 12px;
  }
  .invalid {
    color: var(--gs-error-fg, #f48771);
    font-family: var(--gs-font-mono, ui-monospace, Menlo, monospace);
    font-size: 11px;
    margin: 6px 0 0;
  }
  .ok {
    color: var(--gs-fg);
    font-family: var(--gs-font-mono, ui-monospace, Menlo, monospace);
    font-size: 11px;
    margin: 6px 0 0;
  }
  .hex {
    text-transform: uppercase;
  }
  .tag {
    border-radius: 9999px;
    padding: 0 6px;
    font-size: 10px;
    font-weight: 600;
    margin-left: 6px;
    text-transform: uppercase;
  }
  .tag-tt {
    background: rgba(35, 134, 54, 0.25);
    color: #4ac26b;
  }
  .tag-pt {
    background: rgba(80, 140, 220, 0.25);
    color: #6aa6ff;
  }
</style>
