#!/usr/bin/env python3
from __future__ import annotations
"""Dev server with cache bust avoidance and optional fallback root.
Usage: python scripts/dev_server.py --root build --port 8080
       python scripts/dev_server.py --root build --port 8080 --fallback-root .
"""
import argparse, os, urllib.parse
from http.server import HTTPServer, SimpleHTTPRequestHandler
from functools import partial

class Handler(SimpleHTTPRequestHandler):
    # Optional fallback directory; set by main() on the class.
    fallback_root = None
    # Default query string appended when redirecting "/" to "/index.html".
    default_params = None

    def end_headers(self):
        self.send_header('Cache-Control', 'no-cache, no-store, must-revalidate, max-age=0')
        self.send_header('Pragma', 'no-cache')
        self.send_header('Expires', '0')
        super().end_headers()

    def do_GET(self):  # noqa: N802
        # Redirect bare "/" to "/index.html", preserving query string.
        # When --default-params is set and no query string is present,
        # inject the default parameters so the emulator auto-boots.
        if self.path == '/' or self.path.startswith('/?'):
            if '?' in self.path:
                qs = '?' + self.path.split('?', 1)[1]
            elif self.default_params:
                qs = '?' + self.default_params
            else:
                qs = ''
            self.send_response(302)
            self.send_header('Location', '/index.html' + qs)
            self.end_headers()
            return
        super().do_GET()

    def translate_path(self, path):
        """Resolve path in the main root; fall back to fallback_root if missing."""
        primary = super().translate_path(path)
        if os.path.exists(primary) or not self.fallback_root:
            return primary
        # Strip query/fragment and URL-decode to get the filesystem-relative path.
        clean = urllib.parse.unquote(urllib.parse.urlparse(path).path).lstrip('/')
        fallback = os.path.join(self.fallback_root, clean)
        if os.path.exists(fallback):
            return fallback
        return primary

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='build')
    ap.add_argument('--port', type=int, default=8080)
    ap.add_argument('--fallback-root', default=None,
                    help='Secondary directory to serve files from when not found in --root')
    ap.add_argument('--default-params', default=None,
                    help='Default URL query string appended when redirecting / to /index.html')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    if not os.path.isdir(root):
        raise SystemExit(f"Missing root '{root}'. Run make first.")
    if a.fallback_root:
        Handler.fallback_root = os.path.abspath(a.fallback_root)
    if a.default_params:
        Handler.default_params = a.default_params
    url = f"http://localhost:{a.port}"
    parts = [f"Serving {root}"]
    if a.fallback_root:
        parts.append(f"(fallback: {Handler.fallback_root})")
    parts.append(f"on {url}")
    print(' '.join(parts))
    if a.default_params:
        print(f"Open: {url}/index.html?{a.default_params}")
    httpd = HTTPServer(('localhost', a.port), partial(Handler, directory=root))
    try: httpd.serve_forever()
    except KeyboardInterrupt: print('\nStop')

if __name__ == '__main__':
    main()
