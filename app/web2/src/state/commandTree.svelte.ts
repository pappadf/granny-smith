// The command browser's tree, outside the component: the rows (from
// lib/commandsTree through the shared TreeState), the selection, and what
// following the console marks.  CommandBrowser.svelte renders one and owns
// the details pane and the keyboard.
//
// Following the console: the console's shell.complete answer names the
// node the path token has resolved to, the partial segment typed and the
// candidates that match it.  The browser opens the levels down to that
// node, marks the children matching the partial and dims the rest of that
// level; the marks are derived from one record (`marks`), so they follow
// the level as it is re-read.

import type { CompletionResult } from '@/bus/emulator';
import { covers, invalidate, onMembersChanged } from '@/bus/memberStore';
import {
  ALIASES_KEY,
  aliasGroupKey,
  expand,
  loadAliases,
  rootRows,
  type BrowserRow,
} from '@/lib/commandsTree';
import { pathPrefixes } from '@/lib/objectPath';
import { completionFocus } from '@/lib/pathToken';
import { TreeState } from '@/lib/treeState.svelte';

// What a console update asks of the view.
export type FollowAction =
  | { kind: 'close' } // the input emptied: close the details pane
  | { kind: 'select'; row: BrowserRow; arg: number | null };

interface Marks {
  level: string | null; // the marked level: a row's key, '' for the root members, null for none
  names: string[]; // the candidate names
  partial: string;
}

const NO_MARKS: Marks = { level: null, names: [], partial: '' };

export class CommandTree {
  readonly tree = new TreeState<BrowserRow, BrowserRow[]>({
    root: rootRows,
    load: expand,
    rows: (l) => l,
    // A section's rows sit at its own indent: it is a headline.
    childDepth: (row, depth) => (row.kind === 'section' ? depth : depth + 1),
    // A collection is re-read on every expansion: entries come and go.
    reloadOnOpen: (row) => row.kind === 'collection',
    defaultOpen: (row) => !!row.defaultOpen,
  });

  selectedKey = $state('');
  marks = $state.raw<Marks>(NO_MARKS);

  readonly selected = $derived(this.selectedKey ? this.tree.findRow(this.selectedKey) : undefined);

  // The rows of the marked level, and those of them that match.
  #level = $derived.by((): readonly BrowserRow[] => {
    const { level, partial } = this.marks;
    if (level === null || !partial) return [];
    if (level === '') return this.#rootMembersLoaded();
    const row = this.tree.findRow(level);
    return (row && this.tree.childrenOf(row)) ?? [];
  });
  // Derived afresh, never mutated.
  readonly matchKeys: ReadonlySet<string> = $derived(
    // eslint-disable-next-line svelte/prefer-svelte-reactivity
    new Set(matching(this.#level, this.marks.names).map((r) => r.key)),
  );
  readonly otherKeys: ReadonlySet<string> = $derived(
    // eslint-disable-next-line svelte/prefer-svelte-reactivity
    new Set(this.#level.filter((r) => !this.matchKeys.has(r.key)).map((r) => r.key)),
  );

  #unsubscribe: () => void;
  #followSeq = 0;

  constructor() {
    // A change in the model re-reads the open levels it dropped.
    this.#unsubscribe = onMembersChanged((c) => {
      if (c.reload) void this.tree.refresh();
      else void this.tree.reread((row) => c.dropped.some((p) => covers(p, row.path)));
    });
  }

  dispose(): void {
    this.#unsubscribe();
    this.tree.dispose();
  }

  // Rebuild from the model (a machine booted or went); open levels stay
  // open, and a section the user collapsed stays collapsed.
  async reload(): Promise<void> {
    invalidate('');
    await this.tree.refresh();
  }

  select(row: BrowserRow): void {
    this.selectedKey = row.key;
  }

  // The loaded rows of the sections that hold root members.
  #rootMembersLoaded(): BrowserRow[] {
    return this.tree.rootRows
      .filter((s) => s.rootMembers)
      .flatMap((s) => this.tree.childrenOf(s) ?? []);
  }

  // Open the levels down to `path`, walking the rows by key (a model row's
  // key is its path; a root-level row may be named by its key).  The
  // section holding the first segment opens; `openLast` opens the row at
  // `path` too.  Answers the deepest row reached: the one at `path` when
  // it is shown.
  async openPath(path: string, { openLast = true } = {}): Promise<BrowserRow | undefined> {
    const keys = pathPrefixes(path);
    if (!keys.length) return undefined;
    const tree = this.tree;
    let row: BrowserRow | undefined;
    for (const s of tree.rootRows) {
      if (!s.rootMembers) continue;
      row = (await tree.ensure(s)).find((r) => r.key === keys[0]);
      if (row) {
        await tree.open(s);
        break;
      }
    }
    let found: BrowserRow | undefined;
    for (let i = 0; row; i++) {
      found = row;
      if (i === keys.length - 1) {
        if (openLast) await tree.open(row);
        break;
      }
      await tree.open(row);
      row = tree.childrenOf(row)?.find((r) => r.key === keys[i + 1]);
    }
    return found;
  }

  // Follow a console update: open and mark the level of the path token,
  // and answer the row to select.  An update that a newer one overtook
  // (it awaits the model) answers null before it marks or selects anything.
  async follow(line: string, r: CompletionResult | null): Promise<FollowAction | null> {
    const seq = ++this.#followSeq;
    const stale = () => seq !== this.#followSeq;
    // An emptied input (a command was run, or the line cleared) has nothing
    // to document.
    if (line.trim() === '') {
      this.marks = NO_MARKS;
      return { kind: 'close' };
    }
    if (!r) {
      this.marks = NO_MARKS;
      return null;
    }
    const f = completionFocus(
      line,
      r.span,
      r.candidates.map((c) => c.text),
    );
    if (f.alias !== null) return this.#followAlias(f.alias, f.names, stale);

    // Open the levels of the token; mark the children matching the partial.
    let level: readonly BrowserRow[];
    let levelKey = '';
    if (f.parent) {
      const parent = await this.openPath(f.parent);
      if (stale()) return null;
      if (parent?.key !== f.parent) {
        this.marks = NO_MARKS;
        return null;
      }
      levelKey = parent.key;
      level = this.tree.childrenOf(parent) ?? [];
    } else {
      for (const s of this.tree.rootRows) if (s.rootMembers) await this.tree.ensure(s);
      if (stale()) return null;
      level = this.#rootMembersLoaded();
    }
    this.marks = { level: levelKey, names: f.names, partial: f.partial };
    const hits = f.partial ? matching(level, f.names) : [];

    const method = r.context.method;
    if (method) {
      // In a method's arguments: that method, with the argument marked.
      const row = await this.openPath(method);
      if (stale() || row?.key !== method) return null;
      return { kind: 'select', row, arg: r.context.argIndex };
    }
    if (!hits.length) return null;
    const exact = hits.find((h) => h.word === f.partial) ?? hits[0];
    if (!f.parent) await this.openPath(exact.key, { openLast: false });
    if (stale()) return null;
    return { kind: 'select', row: exact, arg: null };
  }

  async #followAlias(
    name: string,
    names: string[],
    stale: () => boolean,
  ): Promise<FollowAction | null> {
    const tree = this.tree;
    const section = tree.rootRows.find((r) => r.key === ALIASES_KEY);
    if (!section) return null;
    await tree.open(section);
    const all = await loadAliases();
    if (stale()) return null;
    const pick =
      all.find((a) => a.name === name) ??
      (names.length === 1 ? all.find((a) => a.name === names[0]) : undefined);
    if (!pick) return null;
    const group = tree.childrenOf(section)?.find((r) => r.key === aliasGroupKey(pick));
    if (!group) return null;
    await tree.open(group);
    const row = tree.childrenOf(group)?.find((r) => r.key === `alias:${pick.name}`);
    if (!row || stale()) return null;
    return { kind: 'select', row, arg: null };
  }
}

// The rows of a level a candidate names.
function matching(level: readonly BrowserRow[], names: readonly string[]): BrowserRow[] {
  const want = new Set(names);
  return level.filter((r) => r.word && want.has(r.word));
}
