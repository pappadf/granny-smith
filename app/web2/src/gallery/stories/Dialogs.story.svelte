<script lang="ts">
  import ConfirmDialog from '@/components/dialogs/ConfirmDialog.svelte';
  import RenameDialog from '@/components/panel-views/filesystem/RenameDialog.svelte';
  import CreateImageDialog from '@/components/display/CreateImageDialog.svelte';
  import type { StoryProps } from '../registry';

  let { variant }: StoryProps = $props();
  const noop = () => undefined;

  // The rename error shows after a refused commit: press Enter on the field.
  $effect(() => {
    if (variant !== 'rename-error') return;
    const t = setTimeout(() => {
      document
        .getElementById('rename-input')
        ?.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true }));
    }, 50);
    return () => clearTimeout(t);
  });
</script>

{#if variant === 'confirm'}
  <ConfirmDialog
    open
    title="Eject disk?"
    message="The disk is still in use by the emulated machine."
    confirmText="Eject"
    onConfirm={noop}
    onClose={noop}
  />
{:else if variant === 'confirm-danger'}
  <ConfirmDialog
    open
    title="Delete 3 items?"
    message="They will be removed from browser storage. This cannot be undone."
    confirmText="Delete"
    danger
    onConfirm={noop}
    onClose={noop}
  />
{:else if variant === 'rename'}
  <RenameDialog open initial="System 7.5.3.dsk" onSubmit={noop} onClose={noop} />
{:else if variant === 'rename-error'}
  <RenameDialog open initial="bad/name" onSubmit={noop} onClose={noop} />
{:else if variant === 'create-hd'}
  <CreateImageDialog open kind="hd" onClose={noop} onCreated={noop} />
{:else if variant === 'create-fd'}
  <CreateImageDialog open kind="fd" onClose={noop} onCreated={noop} />
{/if}
