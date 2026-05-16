<script lang="ts">
  import { mmuWalk, type WalkResult } from '@/bus/mockMmu';
  import { debug } from '@/state/debug.svelte';
  import { fmtHex32, parseHex } from '@/lib/hex';

  // inputValue mirrors debug.mmuTransAddr but is mutable so the user
  // can type into it before committing with Enter / Walk.
  // eslint-disable-next-line svelte/prefer-writable-derived
  let inputValue = $state(fmtHex32(debug.mmuTransAddr));
  // result is recomputed reactively in the $effect below; it must be a
  // writable $state so the WalkResult's async-ish "compute then assign"
  // pattern works.
  // eslint-disable-next-line svelte/prefer-writable-derived
  let result = $state<WalkResult | null>(null);

  $effect(() => {
    inputValue = fmtHex32(debug.mmuTransAddr);
  });

  // Auto-walk when the address or supervisor mode changes.
  $effect(() => {
    result = mmuWalk(debug.mmuTransAddr, debug.mmuSupervisor);
  });

  function walk() {
    const v = parseHex(inputValue);
    if (v === null) return;
    debug.mmuTransAddr = v;
  }

  function onKey(ev: KeyboardEvent) {
    if (ev.key === 'Enter') {
      ev.preventDefault();
      walk();
    }
  }

  const PRESETS = [0x00400000, 0x00fc0000, 0x47f00000, 0x00000100];
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
    <button type="button" class="btn" onclick={walk}>Walk</button>
    <span class="presets">
      {#each PRESETS as p (p)}
        <button
          type="button"
          class="preset-btn"
          onclick={() => {
            debug.mmuTransAddr = p;
          }}
        >
          ${fmtHex32(p)}
        </button>
      {/each}
    </span>
  </div>
  {#if result}
    {#if !result.valid}
      <p class="invalid">
        L:$<span class="hex">{fmtHex32(result.logical)}</span> → INVALID — page-fault if accessed
      </p>
    {:else if result.kind === 'TT'}
      <p class="ok">
        L:$<span class="hex">{fmtHex32(result.logical)}</span> P:$<span class="hex"
          >{fmtHex32(result.physical ?? 0)}</span
        >
        <span class="tag tag-tt">TT</span>
      </p>
      <p class="root">Root: transparent translation (no walk)</p>
    {:else}
      <p class="ok">
        L:$<span class="hex">{fmtHex32(result.logical)}</span> P:$<span class="hex"
          >{fmtHex32(result.physical ?? 0)}</span
        >
        <span class="tag tag-pt">PT</span>
      </p>
      <p class="root">Root: {result.root}</p>
      {#each result.levels as level (level.idx)}
        <div class="level-card">
          <div class="level-title">
            Level {result.levels.indexOf(level)} (TIA={level.idx}) · index={level.idx}
          </div>
          <div class="level-detail">
            descriptor at $<span class="hex">{fmtHex32(level.descriptorAddr)}</span>: $<span
              class="hex">{fmtHex32(level.descriptorWord)}</span
            >
            · DT={level.dt}
            {#if level.next !== undefined}
              · next=$<span class="hex">{fmtHex32(level.next)}</span>
            {/if}
          </div>
        </div>
      {/each}
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
    font-size: 12px;
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
    font-size: 12px;
    margin: 6px 0 0;
  }
  .ok {
    color: var(--gs-fg);
    font-family: var(--gs-font-mono, ui-monospace, Menlo, monospace);
    font-size: 12px;
    margin: 6px 0 0;
  }
  .hex {
    text-transform: uppercase;
  }
  .root {
    color: var(--gs-fg-muted);
    font-size: 11px;
    margin: 0;
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
  .level-card {
    border-left: 2px solid var(--gs-border);
    padding-left: 10px;
    margin: 4px 0 0 4px;
  }
  .level-title {
    font-size: 11px;
    color: var(--gs-fg-bright);
    font-weight: 600;
  }
  .level-detail {
    font-family: var(--gs-font-mono, ui-monospace, Menlo, monospace);
    font-size: 11px;
    color: var(--gs-fg-muted);
  }
</style>
