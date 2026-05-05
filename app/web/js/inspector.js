// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Object inspector panel (M11). A read-only browser of the gs_eval
// object tree. Idle while the emulator is running; repaints once on
// every "stopped" run-state edge per object-model proposal §7.1.
//
//   tree  : root children + their descendants, expandable on click
//   detail: attributes (table) + methods (chips) for the selected
//           object/path. Values come from gsEval(path) per attribute.
//
// There is no subscribe/poll loop — the panel is fed by:
//   - first paint when the user expands the panel
//   - re-fetch on every run-state false (stop) edge

import { onRunStateChange, isRunning } from './emulator.js';

let _panel = null;
let _toggle = null;
let _treeEl = null;
let _detailEl = null;
let _statusEl = null;
let _selectedPath = null;
let _initialised = false;

// gsEval helpers — return a value or null on failure / running.
async function safeEval(path, args) {
  try {
    if (typeof window.gsEval !== 'function') return null;
    const r = await window.gsEval(path, args);
    if (r && typeof r === 'object' && 'error' in r) return null;
    return r;
  } catch (_) {
    return null;
  }
}

function setStatus(msg) {
  if (_statusEl) _statusEl.textContent = msg || '';
}

function clear(el) { while (el.firstChild) el.removeChild(el.firstChild); }

// Render the value side of an attribute row. Keep it tight — same
// formatting rules as the shell's `format_value_print`.
function formatValue(v) {
  if (v === null || v === undefined) return '∅';
  if (typeof v === 'boolean') return v ? 'true' : 'false';
  if (typeof v === 'number') return String(v);
  if (typeof v === 'string') return v;
  if (Array.isArray(v)) return `<list:${v.length}>`;
  if (typeof v === 'object') {
    if (v.object) return `<${v.object}:${v.name || ''}>`;
    if ('enum' in v) return `${v.enum} (${v.index})`;
  }
  try { return JSON.stringify(v); } catch (_) { return String(v); }
}

// Build the left-side tree rows. Top level = root children. We don't
// recursively expand for now — clicking a row swaps the right-side
// detail to that path; a future iteration can add per-row expand/
// collapse on the same panel.
async function paintTree() {
  if (!_treeEl) return;
  setStatus('refreshing…');
  clear(_treeEl);

  // Root members: methods + attached children. The root's `objects`
  // method returns the list of attached child names; introspection
  // methods (objects/attributes/methods/help/time) and other
  // top-level methods are deliberately omitted from the tree — they
  // operate on the root, not the tree.
  const children = await safeEval('objects', ['']);
  const names = Array.isArray(children) ? children.slice() : [];
  names.sort();
  for (const name of names) {
    appendTreeRow(name);
  }
  if (!names.length) {
    const empty = document.createElement('p');
    empty.className = 'inspector-empty';
    empty.textContent = isRunning() ? 'Running…' : 'No object tree (no machine booted yet).';
    _treeEl.appendChild(empty);
  }
  if (_selectedPath) restoreSelection();
  setStatus('');
}

function appendTreeRow(path) {
  const row = document.createElement('div');
  row.className = 'ip-row';
  row.dataset.path = path;
  row.textContent = path;
  row.setAttribute('role', 'treeitem');
  row.setAttribute('tabindex', '0');
  row.addEventListener('click', () => selectPath(path));
  row.addEventListener('keydown', (e) => {
    if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); selectPath(path); }
  });
  _treeEl.appendChild(row);
}

function restoreSelection() {
  for (const row of _treeEl.querySelectorAll('.ip-row')) {
    row.setAttribute('aria-selected', row.dataset.path === _selectedPath ? 'true' : 'false');
  }
}

async function selectPath(path) {
  _selectedPath = path;
  restoreSelection();
  await paintDetail();
}

async function paintDetail() {
  if (!_detailEl) return;
  clear(_detailEl);
  if (!_selectedPath) {
    const empty = document.createElement('p');
    empty.className = 'inspector-empty';
    empty.textContent = 'Pick a path on the left.';
    _detailEl.appendChild(empty);
    return;
  }
  const heading = document.createElement('h3');
  heading.textContent = _selectedPath;
  _detailEl.appendChild(heading);

  // Attributes (each with current value)
  const attrs = await safeEval('attributes', [_selectedPath]);
  if (Array.isArray(attrs) && attrs.length) {
    const table = document.createElement('table');
    table.className = 'ip-attr-table';
    for (const name of attrs) {
      const tr = document.createElement('tr');
      const td1 = document.createElement('td');
      td1.textContent = name;
      const td2 = document.createElement('td');
      td2.textContent = formatValue(await safeEval(`${_selectedPath}.${name}`));
      tr.appendChild(td1);
      tr.appendChild(td2);
      table.appendChild(tr);
    }
    _detailEl.appendChild(table);
  }

  // Methods (chips, no invocation — clicking is intentionally inert
  // until M12 wires per-method arg dialogs).
  const methods = await safeEval('methods', [_selectedPath]);
  if (Array.isArray(methods) && methods.length) {
    const heading2 = document.createElement('h3');
    heading2.textContent = 'Methods';
    _detailEl.appendChild(heading2);
    const list = document.createElement('div');
    list.className = 'ip-method-list';
    for (const name of methods) {
      const chip = document.createElement('span');
      chip.className = 'ip-method';
      chip.textContent = `${name}()`;
      list.appendChild(chip);
    }
    _detailEl.appendChild(list);
  }

  // Child object names (let the user drill down in one click).
  const childNames = await safeEval('objects', [_selectedPath]);
  if (Array.isArray(childNames) && childNames.length) {
    const heading3 = document.createElement('h3');
    heading3.textContent = 'Children';
    _detailEl.appendChild(heading3);
    const list = document.createElement('div');
    list.className = 'ip-method-list';
    for (const name of childNames) {
      const chip = document.createElement('span');
      chip.className = 'ip-method';
      chip.textContent = name;
      chip.style.cursor = 'pointer';
      chip.addEventListener('click', () => selectPath(`${_selectedPath}.${name}`));
      list.appendChild(chip);
    }
    _detailEl.appendChild(list);
  }
}

function setExpanded(expanded) {
  if (!_panel) return;
  _panel.setAttribute('data-collapsed', expanded ? 'false' : 'true');
  if (_toggle) _toggle.setAttribute('aria-expanded', expanded ? 'true' : 'false');
  if (expanded) paintTree();
}

export function initInspector() {
  if (_initialised) return;
  _panel = document.getElementById('inspector-panel');
  _toggle = document.getElementById('inspector-toggle');
  _treeEl = document.getElementById('inspector-tree');
  _detailEl = document.getElementById('inspector-detail');
  _statusEl = document.getElementById('inspector-status');
  if (!_panel || !_toggle) return;
  _initialised = true;

  const onToggle = () => {
    const collapsed = _panel.getAttribute('data-collapsed') !== 'false';
    setExpanded(collapsed);
  };
  _toggle.addEventListener('click', onToggle);
  _toggle.addEventListener('keydown', (e) => {
    if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); onToggle(); }
  });

  // Repaint on every "stopped" run-state edge (proposal §7.1).
  // Idle while running — value reads would race the worker.
  let prevRunning = false;
  onRunStateChange((running) => {
    const wasStop = prevRunning && !running;
    prevRunning = running;
    if (wasStop && _panel.getAttribute('data-collapsed') === 'false') {
      // Refresh whatever's currently visible.
      if (_selectedPath) paintDetail();
      else paintTree();
    }
  });

  // E2E hook: deterministic refresh + selection. Lets specs assert on
  // the panel without staging a stop/run cycle.
  window.__gsInspector = {
    expand: () => setExpanded(true),
    refresh: refreshInspector,
    select: selectPath,
    getDetailText: () => (_detailEl ? _detailEl.textContent : ''),
    getTreeNames: () => Array.from(_treeEl ? _treeEl.querySelectorAll('.ip-row') : []).map((r) => r.dataset.path),
  };
}

// Test hook: lets E2E specs deterministically refresh without
// stopping/restarting the emulator.
export async function refreshInspector() {
  if (!_initialised) return;
  if (_selectedPath) await paintDetail();
  else await paintTree();
}
