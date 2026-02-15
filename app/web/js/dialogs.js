// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Lazily instantiates modal dialogs from <template> elements in the HTML.
// No external module dependencies — pure DOM manipulation.

// Show upload dialog using template from HTML
export function showUploadDialog(message) {
  let dlg = document.getElementById('upload-dialog');
  if (!dlg) {
    const template = document.getElementById('upload-dialog-template');
    dlg = document.createElement('div');
    dlg.id = 'upload-dialog';
    dlg.className = 'modal';
    dlg.setAttribute('role', 'dialog');
    dlg.setAttribute('aria-modal', 'true');
    dlg.setAttribute('aria-hidden', 'true');
    if (template) {
      dlg.appendChild(template.content.cloneNode(true));
    }
    document.body.appendChild(dlg);
    dlg.addEventListener('click', e => { if (e.target === dlg) hide(); });
    const closeBtn = dlg.querySelector('[data-upload-close]');
    if (closeBtn) closeBtn.addEventListener('click', hide);
    function hide() { dlg.setAttribute('aria-hidden', 'true'); }
  }
  const msgEl = dlg.querySelector('[data-upload-msg]');
  if (msgEl) msgEl.textContent = message;
  dlg.setAttribute('aria-hidden', 'false');
}

// Show checkpoint dialog using template from HTML — returns Promise<boolean>
export function showCheckpointPrompt() {
  return new Promise((resolve) => {
    let dlg = document.getElementById('checkpoint-dialog');
    if (!dlg) {
      const template = document.getElementById('checkpoint-dialog-template');
      dlg = document.createElement('div');
      dlg.id = 'checkpoint-dialog';
      dlg.className = 'modal';
      dlg.setAttribute('role', 'dialog');
      dlg.setAttribute('aria-modal', 'true');
      dlg.setAttribute('aria-hidden', 'true');
      if (template) {
        dlg.appendChild(template.content.cloneNode(true));
      }
      document.body.appendChild(dlg);
    }
    const titleEl = dlg.querySelector('[data-checkpoint-title]');
    const msgEl = dlg.querySelector('[data-checkpoint-message]');
    const continueBtn = dlg.querySelector('[data-checkpoint-continue]');
    const freshBtn = dlg.querySelector('[data-checkpoint-fresh]');
    if (titleEl) titleEl.textContent = 'Continue from saved checkpoint?';
    if (msgEl) msgEl.textContent = 'A saved checkpoint of your emulator session was found. Resume from where you left off, or start fresh with a cold boot.';
    dlg.setAttribute('aria-hidden', 'false');
    let closed = false;
    const cleanup = () => {
      document.removeEventListener('keydown', onKey);
    };
    const close = (result) => {
      if (closed) return;
      closed = true;
      dlg.setAttribute('aria-hidden', 'true');
      cleanup();
      resolve(result);
    };
    const onKey = (ev) => {
      if (ev.key === 'Escape') {
        ev.preventDefault();
        close(false);
      }
    };
    document.addEventListener('keydown', onKey);
    continueBtn?.addEventListener('click', () => close(true), { once: true });
    freshBtn?.addEventListener('click', () => close(false), { once: true });
    dlg.addEventListener('click', (ev) => { if (ev.target === dlg) close(false); });
    setTimeout(() => continueBtn?.focus(), 20);
  });
}
