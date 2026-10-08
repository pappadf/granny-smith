#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) pappadf
from __future__ import annotations
"""Dev server with cache bust avoidance and optional fallback root.
Usage: python scripts/dev_server.py --root build --port 8080
       python scripts/dev_server.py --root build --port 8080 --fallback-root .
"""
import argparse, os, urllib.parse
from http.server import ThreadingHTTPServer, SimpleHTTPRequestHandler
from functools import partial


class Server(ThreadingHTTPServer):
    # Never share a port: the bind is the free-port probe (some Python
    # versions turn SO_REUSEPORT on for HTTPServer).
    allow_reuse_port = False


def serve_on_free_port(handler, preferred, host='localhost', tries=20):
    """A server listening on the preferred port, or the first free one above it.

    Two checkouts of this project served at once (two editors, two
    branches) must not share an origin: OPFS access handles are exclusive
    per origin, the checkpoint machine id lives in localStorage, and a COI
    service worker registered by one would control the other.  A different
    port is a different origin, so `make run` simply takes the next one.
    The server itself is what binds -- a probe socket closed before the
    real bind would leave a window for another process to take the port.
    """
    for port in range(preferred, preferred + tries):
        try:
            return Server((host, port), handler)
        except OSError:
            continue
    raise SystemExit(f"No free port in {preferred}..{preferred + tries - 1}")

class Handler(SimpleHTTPRequestHandler):
    # HTTP/1.1 with keep-alive: the browser closes the connection when it has
    # everything.  Under HTTP/1.0 the server closed right after the last byte,
    # and an editor's port forwarder (VS Code's, between a devcontainer and
    # the host browser) tore the client side down on that EOF before it had
    # drained its buffer -- large assets arrived short, Chrome reported
    # ERR_CONTENT_LENGTH_MISMATCH, and the bytes had in fact all left here.
    protocol_version = 'HTTP/1.1'
    # Optional fallback directory; set by main() on the class.
    fallback_root = None
    # Default query string appended when redirecting "/" to "/index.html".
    default_params = None
    # When True, skip COOP/COEP headers (let the service worker handle them).
    no_coi_headers = False

    def end_headers(self):
        self.send_header('Cache-Control', 'no-cache, no-store, must-revalidate, max-age=0')
        self.send_header('Pragma', 'no-cache')
        self.send_header('Expires', '0')
        if not self.no_coi_headers:
            # Required for SharedArrayBuffer (pthreads) and OPFS SyncAccessHandle
            self.send_header('Cross-Origin-Opener-Policy', 'same-origin')
            self.send_header('Cross-Origin-Embedder-Policy', 'require-corp')
        super().end_headers()

    def do_GET(self):  # noqa: N802
        # Rewrite bare "/" to "/index.html" so the response carries full
        # COOP/COEP headers (302 redirects can lose headers through proxies
        # such as GitHub Codespaces port-forwarding).
        # When --default-params is set and no query string is present,
        # redirect so the browser picks up the default parameters.
        if self.path == '/' or self.path.startswith('/?'):
            if not self.default_params or '?' in self.path:
                # Serve index.html directly (no redirect)
                self.path = '/index.html' if '?' not in self.path else '/index.html?' + self.path.split('?', 1)[1]
                return super().do_GET()
            # Redirect only when injecting default params
            self.send_response(302)
            self.send_header('Location', '/index.html?' + self.default_params)
            self.end_headers()
            return
        super().do_GET()

    def translate_path(self, path):
        """Resolve path in the main root; fall back to fallback_root if missing."""
        primary = super().translate_path(path)
        if os.path.exists(primary) or not self.fallback_root:
            return primary
        # The base class's own resolution against the fallback root: it drops
        # the query, decodes, and discards '..' segments, so a request cannot
        # climb out of the fallback root.
        root, self.directory = self.directory, self.fallback_root
        try:
            fallback = super().translate_path(path)
        finally:
            self.directory = root
        if os.path.exists(fallback):
            return fallback
        return primary

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='build')
    ap.add_argument('--port', type=int, default=8080)
    ap.add_argument('--fallback-root', default=None,
                    help='Secondary directory to serve files from when not found in --root')
    ap.add_argument('--param', action='append', default=[], metavar='KEY=VALUE',
                    help='A default URL query parameter (repeatable), URL-encoded into the '
                         'query string appended when redirecting / to /index.html')
    ap.add_argument('--no-coi-headers', action='store_true',
                    help='Skip COOP/COEP headers (let the service worker inject them instead)')
    ap.add_argument('--strict-port', action='store_true',
                    help='Fail if --port is taken instead of moving to the next free one')
    a = ap.parse_args()
    params = []
    for kv in a.param:
        key, sep, value = kv.partition('=')
        if not sep or not key:
            raise SystemExit(f"--param wants KEY=VALUE, not '{kv}'")
        params.append((key, value))
    # Each value encoded on its own, so a media path holding '&', '%' or a
    # space neither splits the query nor breaks the Location header.
    default_params = urllib.parse.urlencode(params, safe='/', quote_via=urllib.parse.quote)
    root = os.path.abspath(a.root)
    if not os.path.isdir(root):
        raise SystemExit(f"Missing root '{root}'. Run make first.")
    if a.fallback_root:
        Handler.fallback_root = os.path.abspath(a.fallback_root)
    if default_params:
        Handler.default_params = default_params
    if a.no_coi_headers:
        Handler.no_coi_headers = True
    # One thread per connection.  A COOP/COEP page opens several module and
    # worker fetches at once and a pthread worker can hold a connection while
    # it blocks; on the single-threaded HTTPServer any one of those stalled
    # every other request (a reload that never completed).
    handler = partial(Handler, directory=root)
    if a.strict_port:
        try:
            httpd = Server(('localhost', a.port), handler)
        except OSError as e:
            raise SystemExit(f"Cannot listen on port {a.port}: {e.strerror} "
                             "(drop --strict-port to take the next free one)")
    else:
        httpd = serve_on_free_port(handler, a.port)
        port = httpd.server_address[1]
        if port != a.port:
            print(f"Port {a.port} is in use (another instance?); using {port}")
        a.port = port
    httpd.daemon_threads = True
    url = f"http://localhost:{a.port}"
    parts = [f"Serving UI on {url} --"]
    parts.append(f"root {root}")
    if a.fallback_root:
        parts.append(f"(fallback: {Handler.fallback_root})")
    parts.append(f"on {url}")
    print(' '.join(parts))
    if default_params:
        print(f"Open: {url}/index.html?{default_params}")
    try: httpd.serve_forever()
    except KeyboardInterrupt: print('\nStop')

if __name__ == '__main__':
    main()
