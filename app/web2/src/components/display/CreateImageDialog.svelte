<script lang="ts">
  import Modal from '@/components/common/Modal.svelte';
  import Button from '@/components/ui/Button.svelte';
  import RadioGroup from '@/components/ui/RadioGroup.svelte';
  import { gsEval } from '@/bus';
  import { FD_DIR, HD_DIR } from '@/lib/opfsPaths';

  interface Props {
    open: boolean;
    /** 'hd' creates a blank hard disk; 'fd' creates a blank floppy. */
    kind: 'hd' | 'fd';
    /**
     * For kind==='hd': which hard-disk bus the target machine uses. 'scsi'
     * builds a 512-byte/block SCSI image from the drive catalog; 'profile'
     * builds a raw 532-byte/block Lisa/XL ParaPort ProFile image.
     */
    bus?: 'scsi' | 'profile';
    onClose: () => void;
    /** Called with the bare filename of the newly created image. */
    onCreated: (name: string) => void;
  }
  let { open, kind, bus = 'scsi', onClose, onCreated }: Props = $props();

  let isProfile = $derived(kind === 'hd' && bus === 'profile');

  interface HdModel {
    label: string;
    sizeBytes: number;
    mb: number;
  }

  // Standard ProFile capacities, in 532-byte blocks (verified against the Lisa
  // OS source): the 5 MB ProFile is 9728 blocks (the canonical device); the
  // 10 MB drive is the Widget, whose "full disk size is 19448 blocks (pages)"
  // per the LOS installer (APIN-OFFICE.TEXT). Both fall in the LOS ProFile
  // driver's "use actual capacity" range so the OS sizes the volume from them.
  const PROFILE_MODELS = [
    { label: '5 MB ProFile', blocks: 9728, mb: 5 },
    { label: '10 MB Widget', blocks: 19448, mb: 10 },
  ];

  let hdModels = $state<HdModel[]>([]);
  let hdSize = $state(0); // selected size in bytes
  let profileBlocks = $state(PROFILE_MODELS[0].blocks); // selected ProFile size (blocks)
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
    if (open && kind === 'hd' && bus === 'scsi' && modelsState === 'idle') {
      modelsState = 'loading';
      void loadHdModels();
    }
  });

  async function loadHdModels() {
    const raw = await gsEval('machine.scsi.hd_models');
    if (!Array.isArray(raw)) {
      modelsState = 'error';
      return;
    }
    const list: HdModel[] = [];
    for (const entry of raw) {
      // hd_models entries are native {label, vendor, product, size}
      // objects (V_MAP through the gsEval bridge) — no inner JSON.parse.
      if (!entry || typeof entry !== 'object') continue;
      const m = entry as { label?: string; size?: number };
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
      if (isProfile) {
        const model = PROFILE_MODELS.find((p) => p.blocks === profileBlocks) ?? PROFILE_MODELS[0];
        name = `blank_profile_${model.mb}MB_${stamp()}.image`;
        ok =
          (await gsEval('files.profile_create', [`${HD_DIR}/${name}`, String(model.blocks)])) ===
          true;
      } else if (kind === 'hd') {
        if (!hdSize) {
          error = 'No drive sizes available.';
          return;
        }
        const mb = Math.round(hdSize / (1024 * 1024));
        name = `blank_${mb}MB_${stamp()}.dmg`; // UDIF: a few KB, whatever the size
        ok = (await gsEval('files.hd_create', [`${HD_DIR}/${name}`, String(hdSize)])) === true;
      } else {
        const highDensity = fdDensity === '1440K';
        name = `blank_${fdDensity}_${stamp()}.dsk`;
        ok = (await gsEval('files.fd_create', [`${FD_DIR}/${name}`, highDensity])) === true;
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

<Modal
  {open}
  title={kind === 'hd'
    ? isProfile
      ? 'Create Blank ProFile'
      : 'Create Blank Hard Disk'
    : 'Create Blank Floppy'}
  {onClose}
>
  {#if isProfile}
    <p class="dlg-help">Choose a capacity for the new (unformatted) ProFile image.</p>
    <RadioGroup
      name="pf-size"
      label="ProFile capacity"
      bind:value={profileBlocks}
      options={PROFILE_MODELS.map((m) => ({
        value: m.blocks,
        label: `${m.label} (${m.blocks.toLocaleString()} blocks)`,
      }))}
    />
  {:else if kind === 'hd'}
    <p class="dlg-help">Choose a size for the new hard disk image.</p>
    {#if modelsState === 'error'}
      <div class="dlg-error">
        Could not load drive sizes.
        <Button class="dlg-btn" onclick={() => (modelsState = 'idle')}>Retry</Button>
      </div>
    {:else if hdModels.length === 0}
      <div class="dlg-help">Loading drive sizes…</div>
    {:else}
      <RadioGroup
        name="hd-size"
        label="Hard disk size"
        bind:value={hdSize}
        options={hdModels.map((m) => ({ value: m.sizeBytes, label: `${m.mb} MB (${m.label})` }))}
      />
    {/if}
  {:else}
    <p class="dlg-help">Choose a capacity for the new (unformatted) floppy image.</p>
    <RadioGroup
      name="fd-density"
      label="Floppy capacity"
      bind:value={fdDensity}
      options={[
        { value: '800K', label: '800 KB (double density)' },
        { value: '1440K', label: '1.4 MB (high density)' },
      ]}
    />
  {/if}
  {#if error}<div class="dlg-error">{error}</div>{/if}

  {#snippet actions()}
    <Button size="lg" class="dlg-btn" onclick={onClose} disabled={creating}>Cancel</Button>
    <Button
      size="lg"
      variant="primary"
      class="dlg-btn dlg-btn-primary"
      onclick={create}
      busy={creating}
      disabled={creating || (kind === 'hd' && !isProfile && hdModels.length === 0)}
    >
      {creating ? 'Creating…' : 'Create'}
    </Button>
  {/snippet}
</Modal>

<style>
  .dlg-help {
    color: var(--gs-text-muted);
    font-size: var(--gs-font-size-base);
    margin: 0 0 var(--gs-space-3) 0;
  }
  .dlg-error {
    margin-top: var(--gs-space-3);
    color: var(--gs-danger-fg);
    font-size: var(--gs-font-size-sm);
  }
</style>
