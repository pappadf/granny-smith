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
    REFRESH_INTERVAL_MS,
    type Level,
    type SysRow,
  } from '@/lib/systemRows';
  import type { ArgInfo, MemberInfo } from '@/bus/systemTree';
  import { gsEval, isGsError, gsErrorText, whenModuleReady } from '@/bus/emulator';
  import { invalidate, onMembersChanged } from '@/bus/memberStore';
  import { TreeState } from '@/lib/treeState.svelte';
  import { machine } from '@/state/machine.svelte';
  import { systemView } from '@/state/system.svelte';
  import { consoleEcho, consoleSubmit } from '@/state/console.svelte';
  import { formatValue, parseCommit, assignStatement, callStatement } from '@/lib/typeDescriptor';
  import { isContainer } from '@/lib/taggedValue';
  import { openContextMenu, type ContextMenuItem } from '@/components/common/ContextMenu.svelte';
  import ValueEditor from '@/components/common/ValueEditor.svelte';
  import PathField from '@/components/common/PathField.svelte';
  import Modal from '@/components/common/Modal.svelte';
  import TreeItem from '@/components/ui/TreeItem.svelte';
  import Button from '@/components/ui/Button.svelte';
  import SectionHeading from '@/components/ui/SectionHeading.svelte';
  import Hint from '@/components/ui/Hint.svelte';
  import { showNotification } from '@/state/toasts.svelte';
  import { downloadFiles } from '@/bus/fsOps';
  import { sanitizeName } from '@/lib/archive';
  import { copyText } from '@/lib/clipboard';
  import Checkbox from '@/components/ui/Checkbox.svelte';

  // Exported images land here: /opfs is file-backed, so writing one costs no
  // wasm heap (see saveImage).  A directory of its own keeps a 512 MB export
  // out of the image categories the New Machine dialog offers.
  const EXPORT_DIR = '/opfs/exports';

  let selectedKey = $state('');
  let editingKey = $state('');
  let listEl = $state<HTMLUListElement | null>(null);

  // Open levels are kept by path in systemView (a model row's key is its
  // path), so they survive the tab closing.
  const tree = new TreeState<SysRow, Level>({
    root: loadRoot,
    load: loadLevel,
    rows: (l) => l.rows,
    shown: (row) => row.kind === 'divider' || shown(row, systemView.showAdvanced),
    reloadOnOpen: () => true,
    expanded: () => systemView.expanded,
    refreshed: () => {
      if (systemView.reveal) showRevealed();
    },
  });
  const flat = $derived(tree.flat);
  const root = $derived(tree.rootRows);

  // --- loading and refresh --------------------------------------------------------

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

  // Machine up or down: the tree changes shape.
  $effect(() => {
    void machine.status;
    untrack(
      () =>
        void whenModuleReady().then(() => {
          invalidate('');
          return tree.refresh();
        }),
    );
  });

  // A change in the model re-reads the open levels (a reload after a boot
  // keeps them by path).
  const unsubscribe = onMembersChanged(() => void tree.refresh());

  // Every 2 s while the machine runs and the page is in the foreground (the
  // tab being open is this component being mounted).
  let timer: ReturnType<typeof setInterval> | null = null;
  onMount(() => {
    timer = setInterval(() => {
      if (machine.status !== 'running') return;
      if (typeof document !== 'undefined' && document.visibilityState === 'hidden') return;
      void tree.refresh();
    }, REFRESH_INTERVAL_MS);
  });
  onDestroy(() => {
    tree.dispose();
    unsubscribe();
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
    void tree.refresh();
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

  // Methods the tab runs its own way, by the method's full path or its
  // name: `export` saves the image to /opfs and downloads it (saveImage).
  const METHOD_HANDLERS = new Map<string, (path: string, m: MemberInfo) => void>([
    ['export', (path) => void saveImage(path)],
  ]);

  function choose(path: string, m: MemberInfo): void {
    const handler = METHOD_HANDLERS.get(`${path}.${m.name}`) ?? METHOD_HANDLERS.get(m.name);
    if (handler) {
      handler(path, m);
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
    void tree.refresh();
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
      const level = tree.levels[row.key] ?? (await loadLevel(row));
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
        void tree.toggle(cur);
      }
      return;
    }
    if (ev.key === 'Enter' || ev.key === 'F2') {
      ev.preventDefault();
      if (cur.expandable) void tree.toggle(cur);
      else if (editable(cur) && cur.type?.kind === 'bool')
        void commit(cur, formatValue(cur.value, cur.type) === 'true' ? 'false' : 'true');
      else startEdit(cur);
    }
  }
</script>

<div class="system-view">
  <div class="system-toolbar">
    <Checkbox
      class="adv-toggle"
      size="sm"
      label="Advanced"
      checked={systemView.showAdvanced}
      onchange={(v) => (systemView.showAdvanced = v)}
    />
  </div>
  {#if !tree.loaded}
    <Hint class="hint" inset="view">Loading system tree…</Hint>
  {:else if root.length === 0}
    <Hint class="hint" inset="view"
      >No machine is running yet. Start one from the Welcome view.</Hint
    >
  {:else}
    <ul class="sys-tree" role="tree" tabindex="0" bind:this={listEl} onkeydown={onKey}>
      {#each flat as { row, depth } (row.key)}
        {#if row.kind === 'divider'}
          <SectionHeading as="li" level="h4" rule class="group-divider" role="presentation"
            >{row.label}</SectionHeading
          >
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
            aria-level={depth + 1}
            data-path={row.path}
            oncontextmenu={(ev) => void onContextMenu(row, ev)}
          >
            <TreeItem
              class="sys-line"
              role="button"
              tabindex={-1}
              title={row.doc ? `${row.doc}\n${row.path}` : row.path}
              density="compact"
              hover={false}
              {depth}
              kind={row.kind}
              hasChildren={row.expandable}
              {open}
              {selected}
              onclick={() => {
                selectedKey = row.key;
                if (row.expandable) void tree.toggle(row);
              }}
            >
              {#snippet content()}
                <span class="name">{row.label}</span>
                {#if row.kind === 'attr'}
                  <!-- svelte-ignore a11y_click_events_have_key_events, a11y_no_static_element_interactions -->
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
                        <summary class="gs-summary">{formatValue(row.value, row.type)}</summary>
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
              {/snippet}
            </TreeItem>
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
    <Button size="lg" onclick={() => (confirming = null)}>Cancel</Button>
    <Button
      size="lg"
      variant="danger"
      class="danger"
      onclick={() => {
        const p = confirming;
        confirming = null;
        if (p) proceed(p);
      }}>{confirming?.method.verb ?? confirming?.method.name}</Button
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
      {#if form.error}<Hint as="div" class="form-error" tone="error">{form.error}</Hint>{/if}
      <button type="submit" hidden aria-hidden="true"></button>
    </form>
  {/if}
  {#snippet actions()}
    <Button size="lg" onclick={() => (form = null)}>Cancel</Button>
    <Button size="lg" variant="primary" class="primary" onclick={() => void submitForm()}
      >{form?.method.verb ?? form?.method.name}</Button
    >
  {/snippet}
</Modal>

<style>
  .system-view {
    width: 100%;
    height: 100%;
    overflow: auto;
    background: var(--gs-surface-app);
    padding: var(--gs-space-1) 0;
    font-size: var(--gs-font-size-sm);
  }
  .system-toolbar {
    display: flex;
    justify-content: flex-end;
    padding: var(--gs-space-0-5) var(--gs-space-2);
  }
  .sys-tree {
    list-style: none;
    margin: 0;
    padding: 0;
  }
  .sys-tree:focus .sys-row.selected > :global(.sys-line) {
    outline: var(--gs-focus-width) solid var(--gs-focus-ring);
    outline-offset: var(--gs-focus-offset);
  }
  .name {
    flex: none;
    color: var(--gs-text);
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
    gap: var(--gs-space-1);
    font-family: var(--gs-font-mono);
    overflow: hidden;
  }
  .value .text {
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .readonly .value {
    color: var(--gs-text-muted);
  }
  .lock {
    visibility: hidden;
    flex: none;
  }
  .readonly:hover .lock {
    visibility: visible;
  }
  .container summary {
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .container pre {
    margin: var(--gs-space-0-5) 0;
    white-space: pre-wrap;
    font-size: var(--gs-font-size-xs);
  }
  .confirm-doc {
    color: var(--gs-text-muted);
    margin: 0 0 var(--gs-space-2);
  }
  .arg-form {
    display: flex;
    flex-direction: column;
    gap: var(--gs-space-2);
    min-width: 320px;
  }
  .arg {
    display: flex;
    flex-direction: column;
    gap: var(--gs-space-0-5);
  }
  .arg-name {
    font-family: var(--gs-font-mono);
  }
  .arg-type {
    margin-left: var(--gs-space-2);
    color: var(--gs-syntax-type);
    font-size: var(--gs-font-size-xs);
  }
  .arg-doc {
    color: var(--gs-text-muted);
    font-size: var(--gs-font-size-xs);
  }
</style>
