// Startup check of the core's storage: files.persistent is false when the
// worker could not open the browser's storage (OPFS) and /opfs fell back to
// memory.  The page then says so, and keeps saying so (App.svelte's banner),
// before the user does any work: empty folders there used to look like every
// stored file had been lost.

import { gsEval } from '@/bus/emulator';
import { storageHealth } from '@/state/storageHealth.svelte';
import { showNotification } from '@/state/toasts.svelte';

export const VOLATILE_STORAGE_MESSAGE =
  'Browser storage could not be opened: nothing will be saved. ROMs, disks and saved states you add are lost when this page is closed or reloaded.';

// Only an explicit false counts: an answer that is not a boolean (a core
// without the attribute) is taken to be persistent, as before.
export async function checkStoragePersistence(): Promise<boolean> {
  const persistent = (await gsEval('files.persistent')) !== false;
  storageHealth.volatile = !persistent;
  if (!persistent) showNotification(VOLATILE_STORAGE_MESSAGE, 'error');
  return persistent;
}
