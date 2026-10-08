// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// core_init.c
// Process bootstrap (see core_init.h).  This used to be the body of
// shell_init, which is the shell's business only for its own two lines.

#include "core_init.h"

#include "core_singletons.h"
#include "log_context.h"
#include "shell.h"
#include "worker_thread.h"
#include "job/job.h"

#include <stdbool.h>

int core_init(void) {
    static bool done = false;
    if (done)
        return 0;

    // Ordering invariants, top to bottom:
    //
    // 1. worker_thread_latch first: it latches this pthread for the
    //    thread-affinity guard (MODE=debug/sanitize: any gs_eval or shell
    //    entry from another thread then aborts).  Everything below runs on
    //    this thread, so a registration that hands work to the emulator
    //    thread already finds the latch set.
    // 2. log_context_install (the logger's PC/count decorations and trace
    //    capture) before anything that logs.
    // 3. job_layer_init (records this as the emulator thread) before any
    //    module that prints or runs work through the job layer.
    // 4. shell_init (binding store, completion provider) before the root is
    //    populated, so a gs_eval landing during init sees a live provider
    //    and the `shell` node never exists without its interpreter state.
    // 5. root_install_class before the singletons, so the root has its real
    //    class (the top-level methods) before anything is attached to it.
    // 6. The singletons in any order among themselves (each attaches only
    //    its own node), but before root_install, whose cfg-scoped stubs and
    //    install hooks (files.images, ...) hang off nodes they create.
    worker_thread_latch();
    log_context_install();
    job_layer_init();
    if (shell_init() != 0)
        return -1;

    // Install the top-level object-root methods (assert, echo, cp,
    // peeler, rom_probe, …) so JS callers (`gsEval`) and the typed
    // path-form parser can reach them.
    root_install_class();

    // Register process-singleton namespace objects that exist
    // independently of any machine instance: rom, machine and catalog
    // all carry pre-boot surfaces (rom.identify, catalog.vroms.identify,
    // machine.boot, catalog.profile) that callers reach for *before*
    // a machine has been created. The WASM URL-media boot path is the
    // canonical case — drag-drop a Plus ROM, ask rom.identify for the
    // compatible models, then call machine.boot with the answer.
    // Hooking these up in system_create was wrong: the WASM platform
    // doesn't run system_create at startup, so the path-form would
    // fail to resolve until a machine had already booted.
    rom_init();
    machine_init();
    checkpoint_init();
    pacing_init();
    files_init();
    log_class_init();
    catalog_init();
    mouse_class_register();
    // `keyboard` is NOT registered here: it is per machine now, built by
    // system_create (host_input.h).  It needs a scheduler source that lives
    // and dies with the machine, which a process-lifetime facade cannot have.
    screen_class_register();
    scsi_class_register();

    // Install the cfg-scoped namespace stubs (shell, files, ...) with a NULL
    // cfg so their pre-boot surfaces resolve — particularly files.cp and
    // files.find_media, which the URL-media auto-boot path uses *before*
    // machine.boot to copy the downloaded ROM into OPFS and to scan
    // extracted archives for floppy images. system_create will later
    // re-install with the real cfg (root_install handles the cfg-change
    // uninstall + reinstall internally).
    root_install(NULL);

    done = true;
    return 0;
}
