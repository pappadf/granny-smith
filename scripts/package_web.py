#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) pappadf
"""Package the built web UI as a self-hostable zip (a release asset).

Usage: python3 scripts/package_web.py VERSION app/web2/dist out.zip

The zip holds:
  web/            the UI as `make ui2` builds it (index.html, main.mjs, main.wasm, ...)
  dev_server.py   scripts/dev_server.py: serves web/ with the COOP/COEP headers
                  the emulator needs (pthreads, OPFS), optionally with a second
                  directory (--fallback-root) for disk images on the same origin
  README.txt      how to serve it
  VERSION         the release tag

so a project can run a pinned emulator build locally (or in a devcontainer)
without building the WASM core itself.
"""
import os
import sys
import zipfile

README = """Granny Smith {version} -- web build for self-hosting

Serve it with the bundled server (Python 3, no packages needed):

    python3 dev_server.py --root web --port 8080

then open http://localhost:8080/.  The emulator needs cross-origin isolation
(COOP/COEP headers), which dev_server.py sends; a plain static server works
only through the bundled coi-serviceworker.js fallback.

Media can be served from a second directory on the same origin and named in
the URL, e.g.

    python3 dev_server.py --root web --fallback-root ../my-disks \\
        --default-params 'model=lisa&rom=/lisa.rom&hd=/profile.image'

URL parameters (rom=, fd0=, hd0=, model=, ...) are described in
docs/guide/web.md of the Granny Smith repository:
https://github.com/pappadf/granny-smith
"""


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    version, dist, out = sys.argv[1:]
    if not os.path.isfile(os.path.join(dist, 'index.html')):
        sys.exit(f'{dist}: no index.html (run make ui2 first)')
    server = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'dev_server.py')
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as z:
        for top, dirs, files in os.walk(dist):
            dirs.sort()
            for f in sorted(files):
                path = os.path.join(top, f)
                z.write(path, os.path.join('web', os.path.relpath(path, dist)))
        z.write(server, 'dev_server.py')
        z.writestr('README.txt', README.format(version=version))
        z.writestr('VERSION', version + '\n')
    print(f'{out}: {os.path.getsize(out)} bytes')


if __name__ == '__main__':
    main()
