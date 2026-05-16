<script lang="ts">
  import CollapsibleSection from '@/components/common/CollapsibleSection.svelte';
  import Icon from '@/components/common/Icon.svelte';
  import ImageRow from './ImageRow.svelte';
  import { openContextMenu, type ContextMenuItem } from '@/components/common/ContextMenu.svelte';
  import { CATEGORY_LABELS, CATEGORY_ACCEPT, iconForCategory } from '@/lib/iconForFsEntry';
  import { opfs } from '@/bus/opfs';
  import { pickAndUpload } from '@/bus/upload';
  import { gsEval } from '@/bus/emulator';
  import { showNotification } from '@/state/toasts.svelte';
  import type { OpfsEntry, ImageCategory } from '@/bus/types';
  import { images, setMounted } from '@/state/images.svelte';

  interface Props {
    cat: ImageCategory;
    open: boolean;
    onToggle: () => void;
    onMountedChange?: () => void;
  }
  let { cat, open, onToggle, onMountedChange }: Props = $props();

  let entries = $state<OpfsEntry[]>([]);
  let loading = $state(false);

  async function refresh() {
    loading = true;
    try {
      entries = await opfs.scanImages(cat);
    } finally {
      loading = false;
    }
  }

  $effect(() => {
    if (open) void refresh();
  });

  // Mounted-state mirror, kept in state/images.svelte.ts. The C side
  // doesn't push mount/unmount events yet (Phase 7), so this view's own
  // mount/unmount actions are the only source of truth for the badge.
  function isMounted(entry: OpfsEntry): boolean {
    return !!images.mounted[entry.path];
  }

  async function onUploadClick(ev: MouseEvent) {
    ev.stopPropagation();
    await pickAndUpload(CATEGORY_ACCEPT[cat]);
    await refresh();
  }

  function onRowContext(entry: OpfsEntry, ev: MouseEvent) {
    ev.preventDefault();
    const mounted = isMounted(entry);
    const items: ContextMenuItem[] = [];
    if (cat === 'fd' || cat === 'hd' || cat === 'cd') {
      items.push({
        label: mounted ? 'Unmount' : 'Mount',
        action: () => (mounted ? unmount(entry) : mount(entry)),
      });
      items.push({ sep: true });
    }
    items.push({ label: 'Download', action: () => showDownloadToast(entry) });
    items.push({ label: 'Rename', action: () => doRename(entry) });
    items.push({ label: 'Delete', action: () => doDelete(entry), danger: true });
    openContextMenu(items, ev.clientX, ev.clientY);
  }

  async function mount(entry: OpfsEntry) {
    try {
      if (cat === 'fd') {
        await gsEval('floppy.drives[0].insert', [entry.path, false]);
      } else if (cat === 'hd') {
        await gsEval('scsi.attach_hd', [entry.path, 0]);
      } else if (cat === 'cd') {
        await gsEval('scsi.attach_cdrom', [entry.path, 3]);
      }
      setMounted(entry.path, true);
      showNotification(`Mounted '${entry.name}'`, 'info');
      onMountedChange?.();
    } catch {
      showNotification('Mount failed', 'error');
    }
  }

  async function unmount(entry: OpfsEntry) {
    try {
      if (cat === 'fd') {
        await gsEval('floppy.drives[0].eject');
      } else if (cat === 'hd') {
        await gsEval('scsi.detach_hd', [0]);
      } else if (cat === 'cd') {
        await gsEval('scsi.detach_cdrom', [3]);
      }
      setMounted(entry.path, false);
      showNotification(`Unmounted '${entry.name}'`, 'info');
      onMountedChange?.();
    } catch {
      showNotification('Unmount failed', 'error');
    }
  }

  function showDownloadToast(entry: OpfsEntry) {
    showNotification(`Download of '${entry.name}' will land in a later phase`, 'warning');
  }

  async function doRename(entry: OpfsEntry) {
    if (typeof window === 'undefined' || typeof window.prompt !== 'function') return;
    const next = window.prompt('Rename', entry.name);
    if (!next || next === entry.name) return;
    try {
      await opfs.rename(entry.path, next);
      await refresh();
      showNotification(`Renamed to '${next}'`, 'info');
    } catch {
      showNotification('Rename failed', 'error');
    }
  }

  async function doDelete(entry: OpfsEntry) {
    if (typeof window !== 'undefined' && typeof window.confirm === 'function') {
      if (!window.confirm(`Delete '${entry.name}'?`)) return;
    }
    try {
      await opfs.delete(entry.path);
      await refresh();
      showNotification(`Deleted '${entry.name}'`, 'info');
    } catch {
      showNotification('Delete failed', 'error');
    }
  }
</script>

<CollapsibleSection title={CATEGORY_LABELS[cat]} {open} {onToggle} count={entries.length}>
  {#snippet actions()}
    <!-- svelte-ignore a11y_click_events_have_key_events -->
    <span
      class="upload-btn"
      role="button"
      tabindex="-1"
      title="Upload {CATEGORY_LABELS[cat]} image"
      onclick={onUploadClick}
    >
      <Icon name="upload" size={14} />
    </span>
  {/snippet}
  {#if loading && entries.length === 0}
    <p class="empty">Loading…</p>
  {:else if entries.length === 0}
    <p class="empty">No {CATEGORY_LABELS[cat]} images. Click the upload button to add one.</p>
  {:else}
    {#each entries as entry (entry.path)}
      <ImageRow
        name={entry.name}
        icon={iconForCategory(cat)}
        mounted={isMounted(entry)}
        onContextMenu={(ev) => onRowContext(entry, ev)}
      />
    {/each}
  {/if}
</CollapsibleSection>

<style>
  .upload-btn {
    display: inline-flex;
    align-items: center;
    justify-content: center;
    width: 22px;
    height: 22px;
    color: var(--gs-fg-muted);
    opacity: 0;
    transition: opacity 100ms;
    cursor: pointer;
  }
  .upload-btn:hover {
    color: var(--gs-fg-bright);
  }
  /* Reveal the button when the user hovers the section header. */
  :global(.section .header:hover) .upload-btn,
  :global(.section .header:focus-within) .upload-btn {
    opacity: 1;
  }
  .empty {
    color: var(--gs-fg-muted);
    font-size: 12px;
    padding: 6px 28px;
  }
</style>
