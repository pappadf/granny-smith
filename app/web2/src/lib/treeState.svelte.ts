// A lazily loaded tree's view state, shared by the SYSTEM tab and the
// command browser: the root level, the loaded level under each row, which
// rows are open (by key) and the rows on screen (`flat`).
//
// A refresh re-reads the root and every open level top-down, so a level
// whose parent lost its row is dropped; sibling subtrees load in parallel.
// One refresh runs at a time, and a request during one runs once more after
// it.  A level loaded while a refresh is under way (a row opened, a level
// re-read) is kept when the refresh lands.

export interface TreeRow {
  key: string;
  expandable: boolean;
}

export interface TreeSource<Row extends TreeRow, Level> {
  root(): Promise<Level>;
  load(row: Row): Promise<Level>;
  rows(level: Level): readonly Row[];
  // Whether a row is on screen (default: every row).
  shown?(row: Row): boolean;
  // The depth of a row's children (default: one deeper).
  childDepth?(row: Row, depth: number): number;
  // Whether opening a row whose level is loaded reads it again.
  reloadOnOpen?(row: Row): boolean;
  // Whether a row never opened or closed starts open.
  defaultOpen?(row: Row): boolean;
  // Where the open rows are kept (default: the tree's own record).
  expanded?(): Record<string, boolean>;
  // After each refresh lands.
  refreshed?(): void;
}

export interface FlatRow<Row> {
  row: Row;
  depth: number;
}

export class TreeState<Row extends TreeRow, Level> {
  root = $state.raw<Level | null>(null);
  levels = $state.raw<Record<string, Level>>({});
  // The first refresh has landed.
  loaded = $state(false);

  #src: TreeSource<Row, Level>;
  #own = $state<Record<string, boolean>>({});
  #running = false;
  #again = false;
  #disposed = false;
  // Levels written while a refresh runs.
  #touched: Set<string> | null = null;

  constructor(src: TreeSource<Row, Level>) {
    this.#src = src;
  }

  get expanded(): Record<string, boolean> {
    return this.#src.expanded?.() ?? this.#own;
  }

  get rootRows(): readonly Row[] {
    return this.root ? this.#src.rows(this.root) : [];
  }

  isOpen(row: Row): boolean {
    return !!this.expanded[row.key];
  }

  // The loaded rows under `row`, or undefined.
  childrenOf(row: Row): readonly Row[] | undefined {
    const l = this.levels[row.key];
    return l === undefined ? undefined : this.#src.rows(l);
  }

  // The rows on screen, depth-first through the open levels.
  flat = $derived.by((): FlatRow<Row>[] => {
    const out: FlatRow<Row>[] = [];
    const src = this.#src;
    const walk = (rows: readonly Row[], depth: number) => {
      for (const row of rows) {
        if (src.shown && !src.shown(row)) continue;
        out.push({ row, depth });
        const level = this.levels[row.key];
        if (row.expandable && this.expanded[row.key] && level !== undefined)
          walk(src.rows(level), src.childDepth ? src.childDepth(row, depth) : depth + 1);
      }
    };
    walk(this.rootRows, 0);
    return out;
  });

  // The loaded row keyed `key`, anywhere in the tree.
  findRow(key: string): Row | undefined {
    const stack = [...this.rootRows];
    while (stack.length) {
      const r = stack.pop()!;
      if (r.key === key) return r;
      const c = this.childrenOf(r);
      if (c) stack.push(...c);
    }
    return undefined;
  }

  #setLevel(key: string, level: Level): void {
    this.levels = { ...this.levels, [key]: level };
    this.#touched?.add(key);
  }

  // The rows under `row`, loading them (without opening it) if needed.
  async ensure(row: Row): Promise<readonly Row[]> {
    if (!(row.key in this.levels)) {
      const level = await this.#src.load(row);
      if (!(row.key in this.levels)) this.#setLevel(row.key, level);
    }
    return this.childrenOf(row) ?? [];
  }

  async open(row: Row): Promise<void> {
    if (!row.expandable || this.isOpen(row)) return;
    if (!(row.key in this.levels) || this.#src.reloadOnOpen?.(row)) {
      const level = await this.#src.load(row);
      if (this.#disposed) return;
      this.#setLevel(row.key, level);
    }
    this.expanded[row.key] = true;
  }

  close(row: Row): void {
    this.expanded[row.key] = false;
  }

  async toggle(row: Row): Promise<void> {
    if (!row.expandable) return;
    if (this.isOpen(row)) this.close(row);
    else await this.open(row);
  }

  // Re-read the open levels whose row passes `pick`.
  async reread(pick: (row: Row) => boolean): Promise<void> {
    const jobs: Promise<void>[] = [];
    for (const key of Object.keys(this.levels)) {
      const row = this.findRow(key);
      if (!row || !this.isOpen(row) || !pick(row)) continue;
      jobs.push(
        this.#src.load(row).then((level) => {
          if (!this.#disposed) this.#setLevel(key, level);
        }),
      );
    }
    await Promise.all(jobs);
  }

  async refresh(): Promise<void> {
    if (this.#running) {
      this.#again = true;
      return;
    }
    this.#running = true;
    try {
      do {
        this.#again = false;
        await this.#pass();
      } while (this.#again && !this.#disposed);
    } finally {
      this.#running = false;
    }
  }

  async #pass(): Promise<void> {
    // eslint-disable-next-line svelte/prefer-svelte-reactivity -- bookkeeping, not state
    const touched = new Set<string>();
    this.#touched = touched;
    const src = this.#src;
    const next: Record<string, Level> = {};
    let root: Level;
    try {
      root = await src.root();
      const expanded = this.expanded;
      const walk = async (rows: readonly Row[]): Promise<void> => {
        for (const r of rows)
          if (r.expandable && !(r.key in expanded) && src.defaultOpen?.(r)) expanded[r.key] = true;
        const open = rows.filter((r) => r.expandable && expanded[r.key]);
        const loaded = await Promise.all(open.map((r) => src.load(r)));
        open.forEach((r, i) => (next[r.key] = loaded[i]));
        await Promise.all(loaded.map((l) => walk(src.rows(l))));
      };
      await walk(src.rows(root));
    } finally {
      this.#touched = null;
    }
    if (this.#disposed) return;
    for (const k of touched) next[k] = this.levels[k];
    this.root = root;
    this.levels = next;
    this.loaded = true;
    src.refreshed?.();
  }

  dispose(): void {
    this.#disposed = true;
  }
}
