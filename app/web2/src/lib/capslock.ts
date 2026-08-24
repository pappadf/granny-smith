// Caps Lock — the host key mirrored into the guest latch and the ⇪ indicator.
//
// Caps Lock is a mechanically locking key on the emulated keyboards, and
// booting Copland D11E4 requires it latched when the machine powers on.
// Browser key events cannot express "held": macOS delivers a keydown only
// when the host caps state turns ON and a keyup only when it turns OFF,
// other hosts report momentary presses, and the emscripten C callbacks
// cannot see the host state at all.  What the DOM *can* see, on every
// keyboard event, is the truth: `getModifierState('CapsLock')`.
//
// So the model is: the host's caps state IS the latch.  Every keyboard
// event syncs it into machine.capsLock (and the live guest via
// setCapsLock); bus/emulator re-asserts it after every boot/restart so a
// rebuilt machine powers on with the key already down.  The ⇪ status-bar
// button remains a manual override for keyboards without a Caps Lock.
//
// The C key path deliberately leaves CapsLock events unconsumed
// (src/platform/wasm/em_main.c), so this listener is the only writer.

import { machine } from '@/state/machine.svelte';
import { setCapsLock } from '@/bus/emulator';

function sync(e: KeyboardEvent): void {
  const on = e.getModifierState('CapsLock');
  if (on !== machine.capsLock) void setCapsLock(on);
}

// Install the window-level listeners. Keydown AND keyup: on macOS the
// off-transition arrives as a lone keyup, and any other key's events also
// carry the current modifier state, which keeps the indicator honest even
// when the caps transition itself happened while the page lacked focus.
export function startCapsLockSync(): void {
  window.addEventListener('keydown', sync);
  window.addEventListener('keyup', sync);
}
