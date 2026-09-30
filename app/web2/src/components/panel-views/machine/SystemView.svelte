<script lang="ts">
  // The SYSTEM tab: the model's state, editable.  Rows come from
  // lib/systemRows (meta.members with values); values show as the REPL
  // prints them (lib/typeDescriptor).  Editing: double-click a value, or
  // Enter / F2 on the selected row (a bool toggles with one click); Enter
  // commits, Esc cancels.  A literal is written with gsEval and echoed to the
  // console; anything else runs as the statement `<path> = <text>` in the
  // console.  The right-click menu runs the node's methods (destructive ones
  // confirm; methods with arguments open a form) and copies a row's value or
  // path.  Visible levels re-read on the core's state events, after every
  // console job and SYSTEM action, and every 2 s while the machine runs.
  import { onMount, onDestroy, untrack } from 'svelte';
  import {
    loadRoot,
    loadLevel,
    shown,
    invalidateStructure,
    REFRESH_EVENTS,
    RELOAD_EVENT,
    REFRESH_INTERVAL_MS,
    type Level,
    type SysRow,
  } from '@/lib/systemRows';
  import type { ArgInfo, MemberInfo } from '@/bus/systemTree';
  import { gsEval, isGsError, gsErrorText, onCoreEvent, whenModuleReady } from '@/bus/emulator';
  import { machine } from '@/state/machine.svelte';
  import { systemView } from '@/state/system.svelte';
  import { consoleEcho, consoleSubmit, onConsoleJobDone } from '@/state/console.svelte';
  import { formatValue, parseCommit, assignStatement, callStatement } from '@/lib/typeDescriptor';
  import { isContainer } from '@/lib/taggedValue';
  import { openContextMenu, type ContextMenuItem } from '@/components/common/ContextMenu.svelte';
  import ValueEditor from '@/components/common/ValueEditor.svelte';
  import PathField from '@/components/common/PathField.svelte';
  import Modal from '@/components/common/Modal.svelte';
  import Icon from '@/components/common/Icon.svelte';
  import { showNotification } from '@/state/toasts.svelte';
  import { downloadFiles } from '@/bus/fsOps';
  import { sanitizeName } from '@/lib/archive';
  import { copyText } from '@/lib/clipboard';

  // Exported images land here: /opfs is file-backed, so writing one costs no
  // wasm heap (see saveImage).  A directory of its own keeps a 512 MB export
  // out of the image categories the New Machine dialog offers.
  const EXPORT_DIR = '/opfs/exports';

  let root = $state<Level>({ rows: [], methods: [], submenus: [] });
  let levels = $state<Record<string, Level>>({});
  let loading = $state(true);
  let selectedKey = $state('');
  let editingKey = $state('');
  let listEl = $state<HTMLUListElement | null>(null);
  let destroyed = false;

  interface FlatRow {
    row: SysRow;
    depth: number;
  }

  // The rows on screen, depth-first through the open levels.
  const flat = $derived.by(() => {
    const out: FlatRow[] = [];
    const walk = (rows: SysRow[], depth: number) => {
      for (const row of rows) {
        if (row.kind !== 'divider' && !shown(row, systemView.showAdvanced)) continue;
        out.push({ row, depth });
        if (row.expandable && systemView.expanded[row.path] && levels[row.path])
          walk(levels[row.path].rows, depth + 1);
      }
    };
    walk(root.rows, 0);
    return out;
  });

  // --- loading and refresh --------------------------------------------------------

  // Re-read the root and every open level (top-down, so a level whose parent
  // lost it is dropped).  One pass at a time; a request during a pass runs
  // once more after it.
  let running = false;
  let again = false;
  async function refresh(): Promise<void> {
    if (running) {
      again = true;
      return;
    }
    running = true;
    try {
      do {
        again = false;
        await pass();
      } while (again && !destroyed);
    } finally {
      running = false;
    }
  }

  async function pass(): Promise<void> {
    const r = await loadRoot();
    const next: Record<string, Level> = {};
    const walk = async (rows: SysRow[]) => {
      const open = rows.filter((row) => row.expandable && systemView.expanded[row.path]);
      const loaded = await Promise.all(open.map((row) => loadLevel(row)));
      for (let i = 0; i < open.length; i++) next[open[i].path] = loaded[i];
      for (const l of loaded) await walk(l.rows);
    };
    await walk(r.rows);
    if (destroyed) return;
    root = r;
    levels = next;
    loading = false;
    if (systemView.reveal) showRevealed();
  }

  // Select the row another surface asked for, once it is loaded.
  function showRevealed(): void {
    const target = systemView.reveal;
    if (!flat.some((f) => f.row.path === target)) return;
    systemView.reveal = '';
    selectedKey = target;
    requestAnimationFrame(() =>
      listEl?.querySelector('.sys-row.selected')?.scrollIntoView({ block: 'nearest' }),
    );
  }

  async function reload(): Promise<void> {
    invalidateStructure();
    await refresh();
  }

  async function toggle(row: SysRow): Promise<void> {
    if (!row.expandable) return;
    if (systemView.expanded[row.path]) {
      systemView.expanded[row.path] = false;
      return;
    }
    levels[row.path] = await loadLevel(row);
    systemView.expanded[row.path] = true;
  }

  // Machine up or down: the tree changes shape.
  $effect(() => {
    void machine.status;
    untrack(() => void whenModuleReady().then(reload));
  });

  const unsubscribe = onCoreEvent((ev) => {
    const k = `${ev.kind}:${ev.event}`;
    if (k === RELOAD_EVENT) void reload();
    else if (REFRESH_EVENTS.has(k)) void refresh();
  });
  const unsubscribeJobs = onConsoleJobDone(() => void refresh());

  // Every 2 s while the machine runs and the page is in the foreground (the
  // tab being open is this component being mounted).
  let timer: ReturnType<typeof setInterval> | null = null;
  onMount(() => {
    timer = setInterval(() => {
      if (machine.status !== 'running') return;
      if (typeof document !== 'undefined' && document.visibilityState === 'hidden') return;
      void refresh();
    }, REFRESH_INTERVAL_MS);
  });
  onDestroy(() => {
    destroyed = true;
    unsubscribe();
    unsubscribeJobs();
    if (timer) clearInterval(timer);
  });

  // --- editing ------------------------------------------------------------------------

  function editable(row: SysRow): boolean {
    return row.kind === 'attr' && !row.readonly && row.value !== undefined;
  }

  function startEdit(row: SysRow): void {
    if (!editable(row) || row.type?.kind === 'bool') return;
    selectedKey = row.key;
    editingKey = row.key;
  }

  function endEdit(): void {
    editingKey = '';
    listEl?.focus();
  }

  // Commit an edit: a literal through gsEval (echoed), anything else as a
  // console statement (its command entry is the record).  Answers an error
  // to show under the field.
  async function commit(row: SysRow, text: string): Promise<string | null> {
    const c = parseCommit(text, row.type);
    if (c.mode === 'statement') {
      consoleSubmit(`${row.path} = ${text.trim()}`);
      endEdit();
      return null;
    }
    const res = await gsEval(row.path, [c.value]);
    if (isGsError(res)) return gsErrorText(res);
    consoleEcho(assignStatement(row.path, c.value, row.type));
    endEdit();
    void refresh();
    return null;
  }

  // --- methods ----------------------------------------------------------------------

  interface Pending {
    path: string; // the node
    method: MemberInfo;
  }
  let confirming = $state<Pending | null>(null);
  let form = $state<(Pending & { values: string[]; error: string }) | null>(null);

  function argsOf(m: MemberInfo): ArgInfo[] {
    return Array.isArray(m.args) ? m.args : [];
  }

  function nodeLabel(path: string): string {
    return path.slice(Math.max(path.lastIndexOf('.'), path.lastIndexOf('[')) + 1) || path;
  }

  function choose(path: string, m: MemberInfo): void {
    if (m.name === 'export') {
      void saveImage(path);
      return;
    }
    if (m.destructive) confirming = { path, method: m };
    else proceed({ path, method: m });
  }

  function proceed(p: Pending): void {
    const args = argsOf(p.method);
    if (args.length) {
      form = {
        ...p,
        values: args.map((a) => (a.default != null ? formatValue(a.default, a.type) : '')),
        error: '',
      };
      return;
    }
    void run(p, []);
  }

  // The form's texts as argument values: literals by their type, other text
  // as a string for the core to read; trailing empty optionals are left out.
  function formArgs(args: ArgInfo[], values: string[]): unknown[] {
    let n = values.length;
    while (n > 0 && !values[n - 1].trim() && args[n - 1]?.optional) n--;
    return values.slice(0, n).map((t, i) => {
      const c = parseCommit(t, args[i]?.type);
      return c.mode === 'literal' ? c.value : t.trim();
    });
  }

  async function run(p: Pending, args: unknown[]): Promise<string | null> {
    const call = `${p.path}.${p.method.name}`;
    const res = await gsEval(call, args);
    if (isGsError(res)) {
      showNotification(`${p.method.verb ?? p.method.name}: ${gsErrorText(res)}`, 'error');
      return gsErrorText(res);
    }
    consoleEcho(
      callStatement(
        call,
        args,
        argsOf(p.method).map((a) => a.type),
      ),
    );
    if (res !== null && res !== undefined && res !== true)
      showNotification(
        `${p.method.verb ?? p.method.name}: ${formatValue(res, p.method.result)}`,
        'info',
      );
    void refresh();
    return null;
  }

  async function submitForm(): Promise<void> {
    if (!form) return;
    const args = argsOf(form.method);
    const missing = args.findIndex((a, i) => !a.optional && !a.rest && !form!.values[i]?.trim());
    if (missing >= 0) {
      form.error = `${args[missing].name} is required`;
      return;
    }
    const p = form;
    const err = await run(p, formArgs(args, p.values));
    if (err) {
      if (form) form.error = err;
      return;
    }
    form = null;
  }

  // Save-image flow.
  //
  // This used to export to /tmp and then hand the file to root.download, and
  // exporting a hard disk aborted the module outright: /tmp on the WASM build
  // is a MEMORY-backed filesystem, and WASMFS grows a memory file's buffer by
  // reallocating it, so the old and the new buffer are both live at every
  // step and the Emscripten heap never gives the space back.  Writing 512 MB
  // cost 1.29 GB of heap and aborted.
  //
  // So the export goes to /opfs, which is file-backed and costs no heap at
  // all, and the browser gets the OPFS entry as a lazy File: createObjectURL
  // streams it from storage, so the bytes never enter the wasm heap or the JS
  // heap either.
  async function saveImage(target: string) {
    const suggested = (await gsEval(`${target}.filename`)) as string;
    const base = (typeof suggested === 'string' && suggested) || 'disk.img';
    const name = window.prompt('Save image as (filename):', base.split('/').pop() || 'disk.img');
    if (!name) return;

    const dest = `${EXPORT_DIR}/${sanitizeName(name)}`;
    showNotification(`Exporting to ${dest}…`, 'info');
    const ok = await gsEval(`${target}.export`, [dest]);
    if (ok !== true) {
      showNotification(`export failed — ${dest} may already exist; see the console`, 'error');
      return;
    }
    consoleEcho(callStatement(`${target}.export`, [dest]));
    // The file is left in /opfs on purpose: the browser reads it after the
    // click, and a 512 MB disk is worth keeping until the user has it.
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

  // --- context menu --------------------------------------------------------------------

  function methodItem(path: string, m: MemberInfo, prefix = ''): ContextMenuItem {
    const verb = m.verb ?? m.name;
    const dots = argsOf(m).length ? '…' : '';
    return {
      label: `${prefix}${verb}${dots}`,
      danger: !!m.destructive,
      action: () => choose(path, m),
    };
  }

  function methodItems(level: Level | undefined, path: string): ContextMenuItem[] {
    if (!level) return [];
    const visibleTier = (m: MemberInfo) =>
      m.category !== 'internal' && (m.category !== 'advanced' || systemView.showAdvanced);
    const items: ContextMenuItem[] = level.methods
      .filter(visibleTier)
      .map((m) => methodItem(path, m));
    for (const sub of level.submenus) {
      const ms = sub.methods.filter(visibleTier);
      if (!ms.length) continue;
      if (items.length) items.push({ sep: true });
      for (const m of ms) items.push(methodItem(sub.path, m, `${sub.name} ▸ `));
    }
    return items;
  }

  async function onContextMenu(row: SysRow, ev: MouseEvent): Promise<void> {
    ev.preventDefault();
    if (row.kind === 'divider') return;
    selectedKey = row.key;
    const x = ev.clientX;
    const y = ev.clientY;
    let items: ContextMenuItem[] = [];
    if (row.expandable) {
      const level = levels[row.path] ?? (await loadLevel(row));
      items = methodItems(level, row.path);
    }
    if (items.length) items.push({ sep: true });
    if (row.kind === 'attr')
      items.push({
        label: 'Copy value',
        action: () => void copyText(formatValue(row.value, row.type)),
      });
    items.push({ label: 'Copy path', action: () => void copyText(row.path) });
    openContextMenu(items, x, y);
  }

  // --- keyboard -------------------------------------------------------------------------

  function onKey(ev: KeyboardEvent): void {
    if (editingKey) return;
    const rows = flat.filter((f) => f.row.kind !== 'divider');
    if (!rows.length) return;
    const idx = rows.findIndex((f) => f.row.key === selectedKey);
    const cur = idx >= 0 ? rows[idx].row : undefined;
    if (ev.key === 'ArrowDown' || ev.key === 'ArrowUp') {
      ev.preventDefault();
      const next =
        ev.key === 'ArrowDown' ? Math.min(rows.length - 1, idx + 1) : Math.max(0, idx - 1);
      selectedKey = rows[next].row.key;
      listEl?.querySelector('.sys-row.selected')?.scrollIntoView({ block: 'nearest' });
      return;
    }
    if (!cur) return;
    if (ev.key === 'ArrowRight' || ev.key === 'ArrowLeft') {
      if (!cur.expandable) return;
      if (!!systemView.expanded[cur.path] !== (ev.key === 'ArrowRight')) {
        ev.preventDefault();
        void toggle(cur);
      }
      return;
    }
    if (ev.key === 'Enter' || ev.key === 'F2') {
      ev.preventDefault();
      if (cur.expandable) void toggle(cur);
      else if (editable(cur) && cur.type?.kind === 'bool')
        void commit(cur, formatValue(cur.value, cur.type) === 'true' ? 'false' : 'true');
      else startEdit(cur);
    }
  }
</script>

<div class="system-view">
  <div class="system-toolbar">
    <label class="adv-toggle">
      <input
        type="checkbox"
        checked={systemView.showAdvanced}
        onchange={() => (systemView.showAdvanced = !systemView.showAdvanced)}
      />
      Advanced
    </label>
  </div>
  {#if loading}
    <p class="hint">Loading system tree…</p>
  {:else if root.rows.length === 0}
    <p class="hint">No machine is running yet. Start one from the Welcome view.</p>
  {:else}
    <ul class="sys-tree" role="tree" tabindex="0" bind:this={listEl} onkeydown={onKey}>
      {#each flat as { row, depth } (row.key)}
        {#if row.kind === 'divider'}
          <li class="group-divider" role="presentation">{row.label}</li>
        {:else}
          {@const open = !!systemView.expanded[row.path]}
          {@const selected = selectedKey === row.key}
          <li
            class="sys-row kind-{row.kind}"
            class:selected
            class:readonly={row.kind === 'attr' && row.readonly}
            role="treeitem"
            aria-selected={selected}
            aria-expanded={row.expandable ? open : undefined}
            style="--depth: {depth}"
            data-path={row.path}
            oncontextmenu={(ev) => void onContextMenu(row, ev)}
          >
            <!-- svelte-ignore a11y_click_events_have_key_events -->
            <div
              class="sys-line"
              role="button"
              tabindex="-1"
              title={row.doc ? `${row.doc}\n${row.path}` : row.path}
              onclick={() => {
                selectedKey = row.key;
                if (row.expandable) void toggle(row);
              }}
            >
              <span class="twistie" class:open aria-hidden="true">
                {#if row.expandable}<Icon name="chevron" size={12} />{/if}
              </span>
              <span class="name">{row.label}</span>
              {#if row.kind === 'attr'}
                <!-- svelte-ignore a11y_no_static_element_interactions -->
                <span
                  class="value"
                  ondblclick={(ev) => {
                    ev.stopPropagation();
                    startEdit(row);
                  }}
                  onclick={(ev) => ev.stopPropagation()}
                >
                  {#if editingKey === row.key}
                    <ValueEditor
                      type={row.type}
                      value={formatValue(row.value, row.type)}
                      autofocus
                      label={row.path}
                      onCommit={(t) => commit(row, t)}
                      onCancel={endEdit}
                    />
                  {:else if row.type?.kind === 'bool' && row.value !== undefined}
                    <ValueEditor
                      type={row.type}
                      value={formatValue(row.value, row.type)}
                      readonly={!editable(row)}
                      label={row.path}
                      onCommit={(t) => commit(row, t)}
                    />
                  {:else if isContainer(row.value)}
                    <details class="container">
                      <summary>{formatValue(row.value, row.type)}</summary>
                      <pre>{JSON.stringify(row.value, null, 2)}</pre>
                    </details>
                  {:else}
                    <span class="text">{formatValue(row.value, row.type)}</span>
                  {/if}
                  {#if row.readonly}
                    <svg
                      class="lock"
                      viewBox="0 0 16 16"
                      width="10"
                      height="10"
                      aria-label="read-only"
                      ><title>read-only</title><path
                        fill="currentColor"
                        d="M4 7V5a4 4 0 0 1 8 0v2h1v8H3V7h1zm2 0h4V5a2 2 0 0 0-4 0v2z"
                      /></svg
                    >
                  {/if}
                </span>
              {/if}
            </div>
          </li>
        {/if}
      {/each}
    </ul>
  {/if}
</div>

<Modal
  open={!!confirming}
  title={confirming
    ? `${confirming.method.verb ?? confirming.method.name} ${nodeLabel(confirming.path)}?`
    : ''}
  onClose={() => (confirming = null)}
>
  {#if confirming}<p class="confirm-doc">{confirming.method.doc}</p>{/if}
  {#snippet actions()}
    <button type="button" onclick={() => (confirming = null)}>Cancel</button>
    <button
      type="button"
      class="danger"
      onclick={() => {
        const p = confirming;
        confirming = null;
        if (p) proceed(p);
      }}>{confirming?.method.verb ?? confirming?.method.name}</button
    >
  {/snippet}
</Modal>

<Modal
  open={!!form}
  title={form ? `${form.path}.${form.method.name}` : ''}
  onClose={() => (form = null)}
>
  {#if form}
    <p class="confirm-doc">{form.method.doc}</p>
    <form
      class="arg-form"
      onsubmit={(ev) => {
        ev.preventDefault();
        void submitForm();
      }}
    >
      {#each argsOf(form.method) as a, i (a.name)}
        <label class="arg">
          <span class="arg-name"
            >{a.name}{a.optional ? '' : ' *'}<span class="arg-type">{a.type?.kind ?? ''}</span
            ></span
          >
          {#if a.type?.presentation === 'path'}
            <PathField
              value={form.values[i]}
              label={a.name}
              onInput={(t) => form && (form.values[i] = t)}
            />
          {:else}
            <ValueEditor
              type={a.type}
              value={form.values[i]}
              label={a.name}
              onInput={(t) => form && (form.values[i] = t)}
            />
          {/if}
          {#if a.doc}<span class="arg-doc">{a.doc}</span>{/if}
        </label>
      {/each}
      {#if form.error}<p class="form-error" role="alert">{form.error}</p>{/if}
      <button type="submit" hidden aria-hidden="true"></button>
    </form>
  {/if}
  {#snippet actions()}
    <button type="button" onclick={() => (form = null)}>Cancel</button>
    <button type="button" class="primary" onclick={() => void submitForm()}
      >{form?.method.verb ?? form?.method.name}</button
    >
  {/snippet}
</Modal>

<style>
  .system-view {
    width: 100%;
    height: 100%;
    overflow: auto;
    background: var(--gs-bg);
    padding: 4px 0;
    font-size: 12px;
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
  .sys-tree {
    list-style: none;
    margin: 0;
    padding: 0;
    outline: none;
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
  .group-divider:first-child {
    border-top: none;
    margin-top: 0;
  }
  .sys-line {
    display: flex;
    align-items: center;
    gap: 6px;
    padding: 1px 8px 1px calc(8px + var(--depth) * 14px);
    cursor: default;
    min-height: 20px;
  }
  .sys-row.selected > .sys-line {
    background: var(--gs-list-active-bg, rgba(0, 120, 212, 0.25));
  }
  .sys-tree:focus .sys-row.selected > .sys-line {
    outline: 1px solid var(--gs-focus-border, #007fd4);
    outline-offset: -1px;
  }
  .twistie {
    display: inline-flex;
    align-items: center;
    justify-content: center;
    width: 14px;
    flex: none;
    color: var(--gs-fg-muted);
    transform: rotate(-90deg);
    transition: transform 80ms ease-out;
  }
  .twistie.open {
    transform: rotate(0deg);
  }
  .name {
    flex: none;
    color: var(--gs-fg);
  }
  .kind-attr .name {
    color: var(--gs-syntax-attribute);
    font-family: var(--gs-font-mono);
  }
  .kind-entry .name {
    font-family: var(--gs-font-mono);
  }
  .value {
    flex: 1 1 auto;
    min-width: 0;
    display: inline-flex;
    align-items: center;
    gap: 4px;
    font-family: var(--gs-font-mono);
    overflow: hidden;
  }
  .value .text {
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .readonly .value {
    color: var(--gs-fg-muted);
  }
  .lock {
    visibility: hidden;
    flex: none;
  }
  .readonly:hover .lock {
    visibility: visible;
  }
  .container summary {
    cursor: pointer;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .container pre {
    margin: 2px 0;
    white-space: pre-wrap;
    font-size: 11px;
  }
  .hint {
    color: var(--gs-fg-muted);
    font-size: 12px;
    padding: 16px;
    line-height: 1.5;
  }
  .confirm-doc {
    color: var(--gs-fg-muted);
    margin: 0 0 8px;
  }
  .arg-form {
    display: flex;
    flex-direction: column;
    gap: 8px;
    min-width: 320px;
  }
  .arg {
    display: flex;
    flex-direction: column;
    gap: 2px;
  }
  .arg-name {
    font-family: var(--gs-font-mono);
  }
  .arg-type {
    margin-left: 8px;
    color: var(--gs-syntax-type);
    font-size: 11px;
  }
  .arg-doc {
    color: var(--gs-fg-muted);
    font-size: 11px;
  }
  .form-error {
    color: var(--gs-syntax-error);
    margin: 0;
  }
</style>
