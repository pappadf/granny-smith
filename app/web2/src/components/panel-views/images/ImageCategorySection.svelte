<script lang="ts">
  import CollapsibleSection from '@/components/common/CollapsibleSection.svelte';
  import Icon from '@/components/common/Icon.svelte';
  import ImageRow from './ImageRow.svelte';
  import { openContextMenu, type ContextMenuItem } from '@/components/common/ContextMenu.svelte';
  import { CATEGORY_LABELS, CATEGORY_ACCEPT, iconForCategory } from '@/lib/iconForFsEntry';
  import { opfs } from '@/bus/opfs';
  import { pickAndUploadAs, acceptFilesAsCategory } from '@/bus/upload';
  import { attachHardDisk, attachCdrom, insertFloppy, ejectMedia } from '@/bus/media';
  import { showNotification } from '@/state/toasts.svelte';
  import type { OpfsEntry, ImageCategory } from '@/bus/types';
  import type { MediaTypeId } from '@/lib/media';
  import {
    images,
    setMounted,
    isMounted as isPathMounted,
    mountBadge,
    bumpImagesRevision,
  } from '@/state/images.svelte';

  // Map the OpfsEntry category to the upload pipeline's MediaTypeId.
  // The only mismatch is 'cd' (here) vs 'cdrom' (media id).
  function mediaIdFor(cat: ImageCategory): MediaTypeId {
    return cat === 'cd' ? 'cdrom' : (cat as MediaTypeId);
  }

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

  // Mounted-state mirror, kept in state/images.svelte.ts. This view's own
  // insert/eject actions set it, and C's Module.onFloppyChange event clears a
  // floppy badge when the guest ejects the disk on its own.
  function isMounted(entry: OpfsEntry): boolean {
    return isPathMounted(entry.path);
  }

  // Removable media (floppy / CD) is "inserted" / "ejected"; a fixed hard
  // disk is "mounted" / "unmounted".
  const isRemovable = $derived(cat === 'fd' || cat === 'cd');

  async function onUploadClick(ev: MouseEvent) {
    ev.stopPropagation();
    // Category-strict picker: the user is in a specific category
    // section, so we won't auto-route to a different one if the file's
    // size happens to match another type.
    await pickAndUploadAs(mediaIdFor(cat), CATEGORY_ACCEPT[cat]);
    await refresh();
  }

  // Drop target — accepts external file drops anywhere on the section.
  // Validates AS this category and rejects (with a toast) on mismatch.
  //
  // dragenter / dragleave fire when the pointer crosses into ANY
  // descendant of .drop-host (the header chevron, action button, each
  // row, etc.), so a naive boolean would flicker on/off as the user
  // dragged across the section. Use a depth counter and treat the
  // visible state as `depth > 0`.
  let dragDepth = $state(0);
  const dropActive = $derived(dragDepth > 0);

  function isFileDrag(ev: DragEvent): boolean {
    const types = ev.dataTransfer?.types;
    if (!types) return false;
    return Array.from(types).includes('Files');
  }

  function onDragEnter(ev: DragEvent) {
    if (!isFileDrag(ev)) return;
    ev.preventDefault();
    dragDepth++;
  }
  function onDragOver(ev: DragEvent) {
    if (!isFileDrag(ev)) return;
    ev.preventDefault();
    ev.dataTransfer!.dropEffect = 'copy';
  }
  function onDragLeave(ev: DragEvent) {
    if (!isFileDrag(ev)) return;
    if (dragDepth > 0) dragDepth--;
  }
  async function onDrop(ev: DragEvent) {
    dragDepth = 0;
    if (!ev.dataTransfer?.files?.length) return;
    ev.preventDefault();
    const files = Array.from(ev.dataTransfer.files);
    if ((await acceptFilesAsCategory(files, mediaIdFor(cat))) !== null) await refresh();
  }

  function onRowContext(entry: OpfsEntry, ev: MouseEvent) {
    ev.preventDefault();
    const mounted = isMounted(entry);
    const items: ContextMenuItem[] = [];
    if (cat === 'fd' || cat === 'hd' || cat === 'cd') {
      const verb = isRemovable ? (mounted ? 'Eject' : 'Insert') : mounted ? 'Unmount' : 'Mount';
      items.push({
        label: verb,
        action: () => (mounted ? unmount(entry) : mount(entry)),
      });
      items.push({ sep: true });
    }
    items.push({ label: 'Download', action: () => showDownloadToast(entry) });
    items.push({ label: 'Rename', action: () => doRename(entry) });
    items.push({ label: 'Delete', action: () => doDelete(entry), danger: true });
    openContextMenu(items, ev.clientX, ev.clientY);
  }

  // Mount / insert and unmount / eject through the one attach helper
  // (bus/media.ts): a floppy into the first empty drive the machine has, a
  // hard disk into the model's boot bay on whatever bus it is (this attached
  // at the default SCSI id on the first bus, ignoring the Lisa's ProFile and
  // a Network Server's second channel), a CD into the model's CD bay.
  // Every result is checked; an unmount ejects from where the mount put it
  // (it called scsi methods that do not exist and toasted success).
  async function mount(entry: OpfsEntry) {
    const r =
      cat === 'fd'
        ? await insertFloppy(entry.path, false)
        : cat === 'hd'
          ? await attachHardDisk(entry.path)
          : await attachCdrom(entry.path);
    if (!r.ok) {
      showNotification(
        `Couldn't ${isRemovable ? 'insert' : 'mount'} '${entry.name}': ${r.reason}`,
        r.full ? 'warning' : 'error',
      );
      return;
    }
    setMounted(entry.path, r.mount);
    showNotification(`${isRemovable ? 'Inserted' : 'Mounted'} '${entry.name}'`, 'info');
    onMountedChange?.();
  }

  async function unmount(entry: OpfsEntry) {
    const where = images.mounted[entry.path];
    if (!where) return;
    const r = await ejectMedia(where);
    if (!r.ok) {
      showNotification(
        `Couldn't ${isRemovable ? 'eject' : 'unmount'} '${entry.name}': ${r.reason}`,
        'error',
      );
      return;
    }
    setMounted(entry.path, null);
    showNotification(`${isRemovable ? 'Ejected' : 'Unmounted'} '${entry.name}'`, 'info');
    onMountedChange?.();
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
      bumpImagesRevision();
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
      bumpImagesRevision();
      showNotification(`Deleted '${entry.name}'`, 'info');
    } catch {
      showNotification('Delete failed', 'error');
    }
  }
</script>

<!-- svelte-ignore a11y_no_static_element_interactions -->
<div
  class="drop-host"
  class:drop-active={dropActive}
  ondragenter={onDragEnter}
  ondragover={onDragOver}
  ondragleave={onDragLeave}
  ondrop={onDrop}
>
  <CollapsibleSection title={CATEGORY_LABELS[cat]} {open} {onToggle} count={entries.length}>
    {#snippet actions()}
      <button
        type="button"
        class="upload-btn"
        title="Upload {CATEGORY_LABELS[cat]} image"
        aria-label="Upload {CATEGORY_LABELS[cat]} image"
        onclick={onUploadClick}
      >
        <Icon name="upload" size={14} />
      </button>
    {/snippet}
    {#if loading && entries.length === 0}
      <p class="empty">Loading…</p>
    {:else if entries.length === 0}
      <p class="empty">
        No {CATEGORY_LABELS[cat]} images. Drop a file here or click the upload button.
      </p>
    {:else}
      {#each entries as entry (entry.path)}
        <ImageRow
          name={entry.name}
          icon={iconForCategory(cat)}
          badge={mountBadge(entry.path)}
          onContextMenu={(ev) => onRowContext(entry, ev)}
        />
      {/each}
    {/if}
  </CollapsibleSection>
</div>

<style>
  /* Always-visible action affordance — drag-and-drop is the slick
     path but click-to-upload still matters for touch / accessibility,
     so the button shouldn't be hover-gated. Muted by default so it
     doesn't compete with the section title; brightens on hover. */
  .upload-btn {
    display: inline-flex;
    align-items: center;
    justify-content: center;
    width: 22px;
    height: 22px;
    padding: 0;
    border: none;
    background: transparent;
    color: var(--gs-fg-muted);
    opacity: 0.6;
    transition:
      opacity 100ms,
      color 100ms;
    cursor: pointer;
  }
  .upload-btn:hover,
  .upload-btn:focus-visible {
    opacity: 1;
    color: var(--gs-fg-bright);
  }
  .empty {
    color: var(--gs-fg-muted);
    font-size: 12px;
    padding: 6px 28px;
  }
  /* Drop-target affordance — subtle inset border while a file is
     being dragged over the section so the user sees which category
     will accept the drop. */
  .drop-host {
    transition: background 80ms ease-out;
  }
  .drop-host.drop-active {
    background: var(--gs-drop-bg);
    outline: 1px dashed var(--gs-drop-border);
    outline-offset: -2px;
  }
</style>
