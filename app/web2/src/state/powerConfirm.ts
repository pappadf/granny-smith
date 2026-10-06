// Confirmation before the actions that throw away a running session: Shut
// down (toolbar power button, Debug Stop) and Restart (Debug toolbar and
// panel menu).  The power button sits next to Run/Pause, so a slip of the
// mouse used to end the session outright (#242).  "Don't ask again" is
// remembered in localStorage like the other UI preferences.

import { askConfirmWithOptOut } from './dialogs.svelte';
import { machine } from './machine.svelte';

const OPT_OUT_KEY = 'gs-confirm-power-off';

function optedOut(): boolean {
  try {
    return localStorage.getItem(OPT_OUT_KEY) === '1';
  } catch {
    return false;
  }
}

function rememberOptOut(): void {
  try {
    localStorage.setItem(OPT_OUT_KEY, '1');
  } catch {
    // localStorage unavailable (private mode, quota) -- ask again next time.
  }
}

export type PowerAction = 'shutdown' | 'restart';

// Resolve true when the action should go ahead: confirmed, opted out, or
// nothing live to lose (no machine, or one already stopped).
export async function confirmPowerAction(action: PowerAction): Promise<boolean> {
  if (machine.status !== 'running' && machine.status !== 'paused') return true;
  if (optedOut()) return true;
  const name = machine.model ?? 'the machine';
  const shutdown = action === 'shutdown';
  const { ok, optOut } = await askConfirmWithOptOut({
    title: shutdown ? 'Shut Down' : 'Restart',
    message: `${shutdown ? 'Shut down' : 'Restart'} ${name}? Anything not saved in the guest is lost.`,
    confirmText: shutdown ? 'Shut Down' : 'Restart',
    danger: true,
    optOutLabel: "Don't ask again",
  });
  if (ok && optOut) rememberOptOut();
  return ok;
}

// Run `fn` once the action is confirmed.
export async function confirmThen(action: PowerAction, fn: () => Promise<void>): Promise<void> {
  if (await confirmPowerAction(action)) await fn();
}
