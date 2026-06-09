// Shared helpers for Filesystem-tree component tests.

// Minimal stand-in for DataTransfer — jsdom's implementation is incomplete for
// drag tests, so we model just the bits the handlers touch.
export function makeDataTransfer(): DataTransfer {
  const store: Record<string, string> = {};
  return {
    setData: (type: string, val: string) => {
      store[type] = val;
    },
    getData: (type: string) => store[type] ?? '',
    get types() {
      return Object.keys(store);
    },
    files: [] as unknown as FileList,
    items: [] as unknown as DataTransferItemList,
    dropEffect: 'none',
    effectAllowed: 'all',
    setDragImage: () => {},
  } as unknown as DataTransfer;
}

// The text of every visible tree-row label.
export function labels(container: HTMLElement): (string | null)[] {
  return Array.from(container.querySelectorAll('.label')).map((e) => e.textContent);
}

// The tree-row element whose label matches `label` (throws if absent).
export function rowFor(container: HTMLElement, label: string): HTMLElement {
  const row = Array.from(container.querySelectorAll('.tree-row')).find(
    (r) => r.querySelector('.label')?.textContent === label,
  );
  if (!row) throw new Error(`row '${label}' not found`);
  return row as HTMLElement;
}
