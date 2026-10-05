<script lang="ts">
  import { askText, askConfirm } from '@/state/dialogs.svelte';
  import { validateName } from '@/components/panel-views/filesystem/RenameDialog.svelte';
  import Hint from '@/components/ui/Hint.svelte';
  import CollapsibleSection from '@/components/common/CollapsibleSection.svelte';
  import ImageRow from './ImageRow.svelte';
  import { openContextMenu, type ContextMenuItem } from '@/components/common/ContextMenu.svelte';
  import { CATEGORY_LABELS, CATEGORY_ACCEPT, iconForCategory } from '@/lib/iconForFsEntry';
  import { opfs } from '@/bus/opfs';
  import { pickAndUploadAs, acceptFilesAsCategory } from '@/bus/upload';
  import {
    insertFloppy,
    ejectMedia,
    mountImage,
    machineDevices,
    deviceLabel,
    type MachineDevice,
  } from '@/bus/media';
  import { showNotification } from '@/state/toasts.svelte';
  import type { OpfsEntry, ImageCategory } from '@/bus/types';
  import { LARGE_IMPORT_BYTES, type MediaTypeId } from '@/lib/media';
  import { downloadFiles, downloadRawImage } from '@/bus/fsOps';
  import { storedDmgName } from '@/bus/importImage';
  import { gsEval, gsErrorText } from '@/bus/emulator';
  import { startActivity, endActivity, setActivityDetail } from '@/state/activity.svelte';
  import IconButton from '@/components/ui/IconButton.svelte';
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

  // Re-scan when the section opens and whenever anything changes the image
  // store (images.revision): an upload from the Welcome page, the New
  // Machine dialog or the Filesystem tab would otherwise leave an open
  // section showing its old listing.
  $effect(() => {
    void images.revision;
    if (open) void refresh();
  });

  // Mounted-state mirror, kept in state/images.svelte.ts. This view's own
  // insert/eject actions set it, and the core's floppy event clears a
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

  async function onRowContext(entry: OpfsEntry, ev: MouseEvent) {
    ev.preventDefault();
    const mounted = isMounted(entry);
    const items: ContextMenuItem[] = [];
    if (cat === 'fd' || cat === 'hd' || cat === 'cd') {
      const verb = isRemovable ? (mounted ? 'Eject' : 'Insert') : mounted ? 'Unmount' : 'Mount';
      items.push({
        label: verb,
        action: () => (mounted ? unmount(entry) : mount(entry)),
      });
      // With several devices that take it, each one by name.
      const devices = !mounted && cat !== 'fd' ? await machineDevices(cat) : [];
      if (devices.length > 1)
        for (const d of devices)
          items.push({
            label: `${verb} into ${deviceLabel(d)}`,
            disabled: d.present,
            action: () => mount(entry, d),
          });
      items.push({ sep: true });
    }
    items.push({ label: 'Save to computer…', action: () => doDownload(entry) });
    if ((cat === 'hd' || cat === 'cd') && /\.dmg$/i.test(entry.name))
      items.push({ label: 'Export as raw image…', action: () => doDownloadRaw(entry) });
    if ((cat === 'hd' || cat === 'cd') && !/\.dmg$/i.test(entry.name) && !mounted)
      items.push({ label: 'Compact (store as .dmg)', action: () => doCompact(entry) });
    items.push({ label: 'Rename', action: () => doRename(entry) });
    items.push({ label: 'Delete', action: () => doDelete(entry), danger: true });
    openContextMenu(items, ev.clientX, ev.clientY);
  }

  // Mount / insert and unmount / eject through the one attach helper
  // (bus/media.ts): a floppy into the first empty drive the machine has, a
  // hard disk or a CD into `device`, or the running machine's first empty
  // device that takes it (machine.storage: the devices its configuration
  // built, on whatever bus).  Every result is checked; an unmount ejects from
  // where the mount put it.
  async function mount(entry: OpfsEntry, device?: MachineDevice) {
    const r =
      cat === 'fd'
        ? await insertFloppy(entry.path, false)
        : await mountImage(cat === 'hd' ? 'hd' : 'cd', entry.path, device);
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

  async function doDownload(entry: OpfsEntry) {
    startActivity(entry.name, 'Saving');
    try {
      const r = await downloadFiles([entry.path]);
      if (r.failures.length) showNotification(`Could not save '${entry.name}'`, 'error');
    } finally {
      endActivity();
    }
  }

  async function doDownloadRaw(entry: OpfsEntry) {
    startActivity(entry.name, 'Saving');
    try {
      const r = await downloadRawImage(entry.path, (done, total) =>
        setActivityDetail(`${Math.round((100 * done) / total)} %`),
      );
      if (!r.ok && r.error !== 'cancelled')
        showNotification(`Could not export '${entry.name}' as a raw image: ${r.error}`, 'error');
    } finally {
      endActivity();
    }
  }

  // Store a raw image the compact way: convert it to a UDIF beside it (the
  // core checks the result decodes to the same bytes), then remove the raw
  // file.  A saved state that used the raw file by name no longer finds it,
  // so the user is asked first.
  async function doCompact(entry: OpfsEntry) {
    const size = (await gsEval('files.path_size', [entry.path])) as number;
    if (typeof size === 'number' && size <= LARGE_IMPORT_BYTES) {
      showNotification(`'${entry.name}' is small already`, 'info');
      return;
    }
    const ok = await askConfirm({
      title: 'Compact image',
      message: `Store '${entry.name}' compressed as a .dmg and remove the raw file? Saved states that use this disk will no longer find it.`,
      confirmText: 'Compact',
    });
    if (!ok) return;
    const dir = entry.path.replace(/\/[^/]+$/, '');
    let dest = `${dir}/${storedDmgName(entry.name)}`;
    for (let i = 2; (await gsEval('files.path_exists', [dest])) === true; i++)
      dest = `${dir}/${storedDmgName(entry.name).replace(/\.dmg$/, `_${i}.dmg`)}`;
    startActivity(entry.name, 'Compacting');
    try {
      const r = await gsEval('files.convert', [entry.path, dest]);
      if (!r || typeof r !== 'object' || 'error' in (r as object)) {
        showNotification(`Could not compact '${entry.name}': ${gsErrorText(r)}`, 'error');
        return;
      }
      const st = r as { bytes_in?: number; stored_bytes?: number };
      await gsEval('files.rm', [entry.path]);
      showNotification(
        `'${entry.name}' compacted: ${Math.round((st.bytes_in ?? size) / 1048576)} MB disk in ${Math.max(1, Math.round((st.stored_bytes ?? 0) / 1048576))} MB`,
        'info',
      );
      bumpImagesRevision();
      await refresh();
    } finally {
      endActivity();
    }
  }

  async function doRename(entry: OpfsEntry) {
    const next = await askText({
      title: 'Rename',
      label: 'New name',
      initial: entry.name,
      submitText: 'Rename',
      validate: validateName,
    });
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
    const ok = await askConfirm({
      title: 'Delete',
      message: `Delete '${entry.name}'?`,
      confirmText: 'Delete',
      danger: true,
    });
    if (!ok) return;
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
      <IconButton
        class="upload-btn"
        icon="upload"
        iconSize="md"
        tone="panel"
        rest="faded"
        label="Add {CATEGORY_LABELS[cat]} image"
        onclick={onUploadClick}
      />
    {/snippet}
    {#if loading && entries.length === 0}
      <Hint class="empty" inset="list">Loading…</Hint>
    {:else if entries.length === 0}
      <Hint class="empty" inset="list">
        No {CATEGORY_LABELS[cat]} images. Drop a file here, or use the add button.
      </Hint>
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
  /* Drop-target affordance — subtle inset border while a file is
     being dragged over the section so the user sees which category
     will accept the drop. */
  .drop-host {
    transition: background var(--gs-duration-instant) var(--gs-ease-out);
  }
  .drop-host.drop-active {
    background: var(--gs-drop-bg);
    outline: 1px dashed var(--gs-drop-border);
    outline-offset: -2px;
  }
</style>
