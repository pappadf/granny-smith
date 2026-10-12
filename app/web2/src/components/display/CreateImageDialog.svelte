<script lang="ts">
  import Modal from '@/components/common/Modal.svelte';
  import Button from '@/components/ui/Button.svelte';
  import Hint from '@/components/ui/Hint.svelte';
  import RadioGroup from '@/components/ui/RadioGroup.svelte';
  import { gsEval, gsErrorText } from '@/bus';
  import type { BlankDisk } from '@/bus/profile';
  import { FD_DIR, HD_DIR } from '@/lib/opfsPaths';

  interface Props {
    open: boolean;
    /** 'hd' creates a blank hard disk; 'fd' creates a blank floppy. */
    kind: 'hd' | 'fd';
    /**
     * For kind==='hd': the blank disks the target device's bus takes, from
     * the machine's storage tree (bus/profile.ts StorageBus.blank_disks) --
     * the core knows each bus's image format and sizes.
     */
    disks?: BlankDisk[];
    onClose: () => void;
    /** Called with the bare filename of the newly created image. */
    onCreated: (name: string) => void;
  }
  let { open, kind, disks = [], onClose, onCreated }: Props = $props();

  let diskIndex = $state(0); // the chosen entry of `disks`
  let fdDensity = $state<'800K' | '1440K'>('800K');
  let creating = $state(false);
  let error = $state('');

  // A new list (another device) starts at its first size.
  $effect(() => {
    void disks;
    diskIndex = 0;
  });

  // Timestamp keeps generated names unique without a manual rename step.
  function stamp(): number {
    return Date.now();
  }

  async function create() {
    error = '';
    creating = true;
    try {
      let name: string;
      let res: unknown;
      if (kind === 'hd') {
        const disk = disks[diskIndex];
        if (!disk) {
          error = 'This device takes no hard disk.';
          return;
        }
        name = `${disk.name}_${stamp()}${disk.ext}`;
        res = await gsEval(disk.method, [`${HD_DIR}/${name}`, disk.arg]);
      } else {
        const highDensity = fdDensity === '1440K';
        name = `blank_${fdDensity}_${stamp()}.dsk`;
        res = await gsEval('files.fd_create', [`${FD_DIR}/${name}`, highDensity]);
      }
      if (res !== true) {
        error = `Failed to create the disk image: ${gsErrorText(res)}`;
        return;
      }
      onCreated(name);
    } finally {
      creating = false;
    }
  }
</script>

<Modal {open} title={kind === 'hd' ? 'Create Blank Hard Disk' : 'Create Blank Floppy'} {onClose}>
  <div class="dlg-body">
    {#if kind === 'hd'}
      <Hint class="dlg-help">Choose a size for the new hard disk image.</Hint>
      {#if disks.length === 0}
        <Hint as="div" tone="error" class="dlg-error">This device takes no hard disk.</Hint>
      {:else}
        <RadioGroup
          name="hd-size"
          label="Hard disk size"
          bind:value={diskIndex}
          options={disks.map((d, i) => ({ value: i, label: d.label }))}
        />
      {/if}
    {:else}
      <Hint class="dlg-help">Choose a capacity for the new (unformatted) floppy image.</Hint>
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
    {#if error}<Hint as="div" tone="error" class="dlg-error">{error}</Hint>{/if}
  </div>

  {#snippet actions()}
    <Button size="lg" onclick={onClose} disabled={creating}>Cancel</Button>
    <Button
      size="lg"
      variant="primary"
      onclick={create}
      busy={creating}
      disabled={creating || (kind === 'hd' && disks.length === 0)}
    >
      {creating ? 'Creating…' : 'Create'}
    </Button>
  {/snippet}
</Modal>

<style>
  .dlg-body :global(.dlg-help) {
    margin: 0 0 var(--gs-space-3) 0;
  }
  .dlg-body :global(.dlg-error) {
    margin-top: var(--gs-space-3);
  }
</style>
