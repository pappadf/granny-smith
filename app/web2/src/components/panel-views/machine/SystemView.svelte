<script lang="ts">
  import { onMount } from 'svelte';
  import Tree, { type TreeNode } from '@/components/common/Tree.svelte';
  import {
    loadSystemRoots,
    loadSystemChildren,
    loadNodeMethods,
    type SystemTreeNode,
  } from '@/bus/systemTree';
  import { gsEval } from '@/bus/emulator';
  import { machine } from '@/state/machine.svelte';
  import { pathKey } from '@/lib/treePath';
  import { openContextMenu, type ContextMenuItem } from '@/components/common/ContextMenu.svelte';
  import { showNotification } from '@/state/toasts.svelte';
  import { downloadFiles } from '@/bus/fsOps';
  import { sanitizeName } from '@/lib/archive';

  // Exported images land here: /opfs is file-backed, so writing one costs no
  // wasm heap (see saveImage).  A directory of its own keeps a 512 MB export
  // out of the image categories the New Machine dialog offers.
  const EXPORT_DIR = '/opfs/exports';

  let rootNodes = $state<SystemTreeNode[]>([]);
  let expanded = $state<Record<string, boolean>>({});
  let loading = $state(true);
  // §7.2: advanced members are hidden until the user opts in.
  let showAdvanced = $state(false);

  async function refresh() {
    loading = true;
    rootNodes = await loadSystemRoots();
    loading = false;
  }

  onMount(() => {
    void refresh();
  });

  // Re-scan when the machine boots / shuts down so the tree stays faithful.
  $effect(() => {
    void machine.status;
    void refresh();
  });

  async function loadChildren(path: string[]): Promise<TreeNode[]> {
    return (await loadSystemChildren(path, showAdvanced)) as TreeNode[];
  }

  // Toggling Advanced changes which rows resolve, so collapse everything to
  // force a fresh lazy walk on next expand.
  function toggleAdvanced() {
    showAdvanced = !showAdvanced;
    expanded = {};
  }

  // The three §5.1 kinds, drawn under non-interactive dividers (§8.2). The
  // machine subtree leads with no heading; the meta objects sit under an
  // "Emulator" divider and the simulated network under "Network".
  const machineNodes = $derived(rootNodes.filter((n) => n.group === 'machine') as TreeNode[]);
  const emulatorNodes = $derived(rootNodes.filter((n) => n.group === 'emulator') as TreeNode[]);
  const networkNodes = $derived(rootNodes.filter((n) => n.group === 'network') as TreeNode[]);

  // Run `target.method` and report the outcome. Mutating calls refresh the
  // tree so the new state shows immediately.
  async function invokeMethod(target: string, method: string, args: unknown[], mutate: boolean) {
    const res = await gsEval(`${target}.${method}`, args);
    if (res && typeof res === 'object' && 'error' in (res as Record<string, unknown>)) {
      showNotification(`${method}: ${(res as { error: string }).error}`, 'error');
      return;
    }
    showNotification(`${method} ok`, 'info');
    if (mutate) void refresh();
  }

  // Save-image flow (§8.4).
  //
  // This used to export to /tmp and then hand the file to root.download, and
  // exporting a hard disk aborted the module outright.  The instrumented run
  // (17 September) showed why, and it was not the read-back copy: /tmp on the
  // WASM build is a MEMORY-backed filesystem, and WASMFS grows a memory file's
  // buffer by reallocating it, so the old and the new buffer are both live at
  // every step and the Emscripten heap never gives the space back.  Writing
  // 512 MB cost 1.29 GB of heap --
  //
  //   331.1 -> 397.3 -> 523.0 -> 779.0 -> 1290.9 MB, Aborted() at 496 of 512
  //
  // -- and the next growth did not fit in the 757 MB left under wasm32's 2 GB
  // ceiling.  A failure to grow is abort(), which is the bare "Uncaught
  // RuntimeError: Aborted()" with nothing else to go on.
  //
  // So the export goes to /opfs, which is file-backed and costs no heap at
  // all, and the browser gets the OPFS entry as a lazy File: createObjectURL
  // streams it from storage, so the bytes never enter the wasm heap or the JS
  // heap either.  root.download is deliberately not used -- it exists to read
  // a file into memory and hand over a copy, which is the thing to avoid.
  async function saveImage(target: string) {
    const suggested = (await gsEval(`${target}.filename`)) as string;
    const base = (typeof suggested === 'string' && suggested) || 'disk.img';
    const name = window.prompt('Save image as (filename):', base.split('/').pop() || 'disk.img');
    if (!name) return;

    const dest = `${EXPORT_DIR}/${sanitizeName(name)}`;
    showNotification(`Exporting to ${dest}…`, 'info');
    const ok = await gsEval(`${target}.export`, [dest]);
    if (ok !== true) {
      showNotification(`export failed — ${dest} may already exist; see the terminal log`, 'error');
      return;
    }

    // Hand the OPFS entry to the browser without reading it.  The file is left
    // in /opfs on purpose: revoking the URL or deleting the file too early
    // truncates the download (the browser reads it after the click), and a
    // 512 MB disk is worth keeping until the user has it.
    const res = await downloadFiles([dest]);
    if (res.failures.length) {
      showNotification(
        `Exported to ${dest}, but the download failed — save it from Files`,
        'error',
      );
      return;
    }
    showNotification(`Exported ${name} (also kept at ${dest})`, 'info');
  }

  // Build the right-click menu for a node from meta.methods (§8.3): one item
  // per UI-surfaced method, destructive ones flagged, args prompted.
  async function onContextMenu(path: string[], ev: MouseEvent) {
    ev.preventDefault();
    const target = path[path.length - 1];
    const methods = await loadNodeMethods(target);
    if (!methods.length) return;
    const items: ContextMenuItem[] = methods.map((info) => ({
      label: info.verb,
      danger: info.destructive,
      action: () => {
        void (async () => {
          if (info.destructive && !window.confirm(`${info.verb}?\n\n${info.doc}`)) return;
          if (info.name === 'export') {
            await saveImage(target);
            return;
          }
          let args: unknown[] = [];
          if (info.nargs > 0) {
            const raw = window.prompt(
              `${info.verb}\n${info.doc}\n\nArguments (space-separated):`,
              '',
            );
            if (raw === null) return;
            args = raw.length ? raw.split(/\s+/) : [];
          }
          await invokeMethod(target, info.name, args, info.mutate);
        })();
      },
    }));
    openContextMenu(items, ev.clientX, ev.clientY);
  }

  function onToggle(p: string[]) {
    const k = pathKey(p);
    expanded[k] = !expanded[k];
  }
</script>

<div class="system-view">
  <div class="system-toolbar">
    <label class="adv-toggle">
      <input type="checkbox" checked={showAdvanced} onchange={toggleAdvanced} />
      Advanced
    </label>
  </div>
  {#if loading}
    <p class="hint">Loading system tree…</p>
  {:else if rootNodes.length === 0}
    <p class="hint">No machine is running yet. Start one from the Welcome view.</p>
  {:else}
    {#if machineNodes.length}
      <Tree nodes={machineNodes} {expanded} {onToggle} {onContextMenu} {loadChildren} />
    {/if}
    {#if emulatorNodes.length}
      <div class="group-divider">Emulator</div>
      <Tree nodes={emulatorNodes} {expanded} {onToggle} {onContextMenu} {loadChildren} />
    {/if}
    {#if networkNodes.length}
      <div class="group-divider">Network</div>
      <Tree nodes={networkNodes} {expanded} {onToggle} {onContextMenu} {loadChildren} />
    {/if}
  {/if}
</div>

<style>
  .system-view {
    width: 100%;
    height: 100%;
    overflow: auto;
    background: var(--gs-bg);
    padding: 4px 0;
  }
  .system-toolbar {
    display: flex;
    justify-content: flex-end;
    padding: 2px 8px;
  }
  .adv-toggle {
    font-size: 11px;
    color: var(--gs-fg-muted);
    display: inline-flex;
    align-items: center;
    gap: 4px;
    cursor: pointer;
  }
  .group-divider {
    font-size: 10px;
    text-transform: uppercase;
    letter-spacing: 0.08em;
    color: var(--gs-fg-muted);
    padding: 8px 12px 2px;
    border-top: 1px solid var(--gs-border, rgba(127, 127, 127, 0.2));
    margin-top: 4px;
  }
  .hint {
    color: var(--gs-fg-muted);
    font-size: 12px;
    padding: 16px;
    line-height: 1.5;
  }
</style>
