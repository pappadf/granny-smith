// Whether the browser storage the core mounts as /opfs is real.  When the
// worker cannot open OPFS it mounts /opfs on memory so the page still works,
// but nothing written there survives the page: every ROM, disk image and
// saved state is lost on reload.  `volatile` is set once at startup
// (bus/storageHealth.ts) and App.svelte keeps a banner up while it is true.

export const storageHealth = $state({ volatile: false });
