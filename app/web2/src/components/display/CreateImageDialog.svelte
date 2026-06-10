<script lang="ts">
  import Modal from '@/components/common/Modal.svelte';
  import { gsEval } from '@/bus';
  import { FD_DIR, HD_DIR } from '@/lib/opfsPaths';

  interface Props {
    open: boolean;
    /** 'hd' creates a blank SCSI hard disk; 'fd' creates a blank floppy. */
    kind: 'hd' | 'fd';
    onClose: () => void;
    /** Called with the bare filename of the newly created image. */
    onCreated: (name: string) => void;
  }
  let { open, kind, onClose, onCreated }: Props = $props();

  interface HdModel {
    label: string;
    sizeBytes: number;
    mb: number;
  }

  let hdModels = $state<HdModel[]>([]);
  let hdSize = $state(0); // selected size in bytes
  let fdDensity = $state<'800K' | '1440K'>('800K');
  let creating = $state(false);
  let error = $state('');

  // Catalog-load state machine. The effect fires the load exactly once per
  // 'idle' — it must NOT key off hdModels.length, because loadHdModels
  // reassigns hdModels (a fresh proxy even when empty), which would re-run
  // the effect immediately: with the emulator still starting (gsEval → null)
  // that loop spins on the microtask queue and freezes the tab.
  let modelsState = $state<'idle' | 'loading' | 'error' | 'ready'>('idle');

  // Load the SCSI drive catalog when the HD dialog opens. The list comes
  // back as JSON strings (V_LIST<V_STRING>); dedupe by label keeping the
  // largest size, matching the legacy dialog.
  $effect(() => {
    if (open && kind === 'hd' && modelsState === 'idle') {
      modelsState = 'loading';
      void loadHdModels();
    }
  });

  async function loadHdModels() {
    const raw = await gsEval('scsi.hd_models');
    if (!Array.isArray(raw)) {
      modelsState = 'error';
      return;
    }
    const list: HdModel[] = [];
    for (const entry of raw) {
      if (typeof entry !== 'string') continue;
      try {
        const m = JSON.parse(entry) as { label?: string; size?: number };
        if (!m.label || !m.size) continue;
        const existing = list.find((x) => x.label === m.label);
        if (existing) {
          if (m.size > existing.sizeBytes) {
            existing.sizeBytes = m.size;
            existing.mb = Math.round(m.size / (1024 * 1024));
          }
        } else {
          list.push({ label: m.label, sizeBytes: m.size, mb: Math.round(m.size / (1024 * 1024)) });
        }
      } catch {
        /* skip malformed entry */
      }
    }
    hdModels = list;
    hdSize = list[0]?.sizeBytes ?? 0;
    modelsState = list.length ? 'ready' : 'error';
  }

  // Timestamp keeps generated names unique without a manual rename step.
  function stamp(): number {
    return Date.now();
  }

  async function create() {
    error = '';
    creating = true;
    try {
      let name: string;
      let ok: boolean;
      if (kind === 'hd') {
        if (!hdSize) {
          error = 'No drive sizes available.';
          return;
        }
        const mb = Math.round(hdSize / (1024 * 1024));
        name = `blank_${mb}MB_${stamp()}.img`;
        ok = (await gsEval('storage.hd_create', [`${HD_DIR}/${name}`, String(hdSize)])) === true;
      } else {
        const highDensity = fdDensity === '1440K';
        name = `blank_${fdDensity}_${stamp()}.dsk`;
        ok = (await gsEval('storage.fd_create', [`${FD_DIR}/${name}`, highDensity])) === true;
      }
      if (!ok) {
        error = 'Failed to create the disk image.';
        return;
      }
      onCreated(name);
    } finally {
      creating = false;
    }
  }
</script>

<Modal {open} title={kind === 'hd' ? 'Create Blank Hard Disk' : 'Create Blank Floppy'} {onClose}>
  {#if kind === 'hd'}
    <p class="dlg-help">Choose a size for the new hard disk image.</p>
    {#if modelsState === 'error'}
      <div class="dlg-error">
        Could not load drive sizes.
        <button type="button" class="dlg-btn" onclick={() => (modelsState = 'idle')}>Retry</button>
      </div>
    {:else if hdModels.length === 0}
      <div class="dlg-help">Loading drive sizes…</div>
    {:else}
      <div class="dlg-options" role="radiogroup">
        {#each hdModels as m (m.label)}
          <label class="dlg-option">
            <input type="radio" name="hd-size" value={m.sizeBytes} bind:group={hdSize} />
            <span>{m.mb} MB ({m.label})</span>
          </label>
        {/each}
      </div>
    {/if}
  {:else}
    <p class="dlg-help">Choose a capacity for the new (unformatted) floppy image.</p>
    <div class="dlg-options" role="radiogroup">
      <label class="dlg-option">
        <input type="radio" name="fd-density" value="800K" bind:group={fdDensity} />
        <span>800 KB (double density)</span>
      </label>
      <label class="dlg-option">
        <input type="radio" name="fd-density" value="1440K" bind:group={fdDensity} />
        <span>1.4 MB (high density)</span>
      </label>
    </div>
  {/if}
  {#if error}<div class="dlg-error">{error}</div>{/if}

  {#snippet actions()}
    <button type="button" class="dlg-btn" onclick={onClose} disabled={creating}>Cancel</button>
    <button
      type="button"
      class="dlg-btn dlg-btn-primary"
      onclick={create}
      disabled={creating || (kind === 'hd' && hdModels.length === 0)}
    >
      {creating ? 'Creating…' : 'Create'}
    </button>
  {/snippet}
</Modal>

<style>
  .dlg-help {
    color: var(--gs-fg-muted);
    font-size: 13px;
    margin: 0 0 12px 0;
  }
  .dlg-options {
    display: flex;
    flex-direction: column;
    gap: 8px;
  }
  .dlg-option {
    display: flex;
    align-items: center;
    gap: 8px;
    font-size: 13px;
    color: var(--gs-fg);
    cursor: pointer;
  }
  .dlg-error {
    margin-top: 12px;
    color: var(--gs-toast-error);
    font-size: 12px;
  }
  .dlg-btn {
    background: var(--gs-input-bg);
    color: var(--gs-input-fg);
    border: 1px solid var(--gs-input-border);
    border-radius: 2px;
    padding: 5px 12px;
    font-size: 13px;
    cursor: pointer;
  }
  .dlg-btn:disabled {
    opacity: 0.5;
    cursor: default;
  }
  .dlg-btn-primary {
    background: var(--gs-primary-bg);
    color: var(--gs-primary-fg);
    border: none;
  }
  .dlg-btn-primary:hover:not(:disabled) {
    background: var(--gs-primary-hover);
  }
</style>
