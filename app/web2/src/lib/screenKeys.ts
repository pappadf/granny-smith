// Browser defaults while the screen has the keyboard, and leaving a live
// machine (#352).
//
// The machine's key callbacks (src/platform/wasm/em_main.c key_down_cb /
// key_up_cb) are registered from the emulator pthread (PROXY_TO_PTHREAD).
// Emscripten's DOM listener then only queues the event to that thread
// (libhtml5.js registerKeyEventCallback: _emscripten_run_callback_on_thread)
// and never sees the callback's answer, so returning EM_TRUE there cannot
// call preventDefault(): F5 reloaded the page, Ctrl-D bookmarked it.  The
// decision has to be made here, on the main thread, while the event is still
// being dispatched.
//
// "The screen has the keyboard" is the grab: pointer lock on #screen
// (ScreenView grab()), the same state the C side gates key routing on
// (pointer_locked).  While it holds, every key's default is suppressed; the
// event still propagates, so Emscripten's document listener sees it and the
// proxied callback keeps doing the routing.
//
// Some shortcuts (Ctrl-W, Ctrl-Tab, Cmd-Q) the browser handles before the
// page in a normal tab, and preventDefault cannot stop them; for those, and
// for a reload from the toolbar, a live machine asks before the page goes.

import { machine } from '@/state/machine.svelte';

// The id of the canvas the machine draws on and grabs input with.
const SCREEN_ID = 'screen';

// True while the emulated screen holds the pointer lock (and so the keyboard).
export function screenHasKeyboard(doc: Document = document): boolean {
  const el = doc.pointerLockElement;
  return !!el && el.id === SCREEN_ID;
}

// True while there is a machine whose session a reload would lose.  A paused
// machine is as live as a running one: only no machine, a stopped one or a
// crashed one has nothing left to lose.
export function machineIsLive(): boolean {
  return machine.status === 'running' || machine.status === 'paused';
}

// Suppress the browser's default action for a key the grabbed screen receives.
function suppressDefault(e: KeyboardEvent): void {
  if (screenHasKeyboard()) e.preventDefault(); // no stopPropagation: the core's listener still sees it
}

// Ask before unloading while a machine is live.
function confirmLeave(e: BeforeUnloadEvent): void {
  if (!machineIsLive()) return;
  e.preventDefault(); // the standard way to ask
  e.returnValue = ''; // older Chromium and Safari ask only when this is set
}

// Install the window listeners; returns a function that removes them.  The
// key listeners are capture-phase on window, so they run before any page
// handler (Emscripten's is capture-phase on document) can stop the event.
export function startScreenKeyGuard(win: Window = window): () => void {
  win.addEventListener('keydown', suppressDefault, true);
  win.addEventListener('keyup', suppressDefault, true);
  win.addEventListener('beforeunload', confirmLeave);
  return () => {
    win.removeEventListener('keydown', suppressDefault, true);
    win.removeEventListener('keyup', suppressDefault, true);
    win.removeEventListener('beforeunload', confirmLeave);
  };
}
