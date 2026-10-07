// In-app questions in place of the browser's prompt() and confirm(): one at
// a time, answered through a promise.  DialogHost (mounted by App) renders
// the open question; askText / askConfirm resolve when it is answered.

export interface TextQuestion {
  kind: 'text';
  title: string;
  label: string;
  initial: string;
  submitText: string;
  validate?: (value: string) => string;
  resolve: (value: string | null) => void;
}

export interface ConfirmQuestion {
  kind: 'confirm';
  title: string;
  message: string;
  confirmText: string;
  danger: boolean;
  // A "Don't ask again" style checkbox under the message, when set.
  optOutLabel?: string;
  resolve: (ok: boolean, optOut?: boolean) => void;
}

export type Question = TextQuestion | ConfirmQuestion;

export const dialogs = $state<{ current: Question | null }>({ current: null });

// Settle any open question as cancelled before asking a new one.
function cancelCurrent(): void {
  const q = dialogs.current;
  if (!q) return;
  dialogs.current = null;
  if (q.kind === 'text') q.resolve(null);
  else q.resolve(false);
}

// Ask for one line of text; null when cancelled (or left empty).
export function askText(opts: {
  title: string;
  label: string;
  initial?: string;
  submitText?: string;
  validate?: (value: string) => string;
}): Promise<string | null> {
  cancelCurrent();
  return new Promise((resolve) => {
    dialogs.current = {
      kind: 'text',
      title: opts.title,
      label: opts.label,
      initial: opts.initial ?? '',
      submitText: opts.submitText ?? 'OK',
      validate: opts.validate,
      resolve,
    };
  });
}

// Ask a yes/no question; true when confirmed.
export function askConfirm(opts: {
  title?: string;
  message: string;
  confirmText?: string;
  danger?: boolean;
}): Promise<boolean> {
  cancelCurrent();
  return new Promise((resolve) => {
    dialogs.current = {
      kind: 'confirm',
      title: opts.title ?? 'Confirm',
      message: opts.message,
      confirmText: opts.confirmText ?? 'OK',
      danger: opts.danger ?? false,
      resolve,
    };
  });
}

// Ask a yes/no question with an opt-out checkbox ("Don't ask again"):
// `ok` is whether it was confirmed, `optOut` whether the box was ticked
// (only reported on a confirm -- a cancel never opts out).
export function askConfirmWithOptOut(opts: {
  title?: string;
  message: string;
  confirmText?: string;
  danger?: boolean;
  optOutLabel: string;
}): Promise<{ ok: boolean; optOut: boolean }> {
  cancelCurrent();
  return new Promise((resolve) => {
    dialogs.current = {
      kind: 'confirm',
      title: opts.title ?? 'Confirm',
      message: opts.message,
      confirmText: opts.confirmText ?? 'OK',
      danger: opts.danger ?? false,
      optOutLabel: opts.optOutLabel,
      resolve: (ok, optOut) => resolve({ ok, optOut: ok && !!optOut }),
    };
  });
}

// Answer the open question (DialogHost).
export function answerText(value: string | null): void {
  const q = dialogs.current;
  if (q?.kind !== 'text') return;
  dialogs.current = null;
  q.resolve(value);
}

export function answerConfirm(ok: boolean, optOut = false): void {
  const q = dialogs.current;
  if (q?.kind !== 'confirm') return;
  dialogs.current = null;
  q.resolve(ok, optOut);
}
