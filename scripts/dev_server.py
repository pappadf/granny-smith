#!/usr/bin/env python3
from __future__ import annotations
"""Dev server with cache bust avoidance.
Usage: python scripts/dev_server.py --root build --port 8080
"""
import argparse, os
from http.server import HTTPServer, SimpleHTTPRequestHandler
from functools import partial

class Handler(SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header('Cache-Control', 'no-cache, no-store, must-revalidate, max-age=0')
        self.send_header('Pragma', 'no-cache')
        self.send_header('Expires', '0')
        super().end_headers()
    def do_GET(self):  # noqa: N802
        if self.path == '/' or self.path.startswith('/?'):
            self.send_response(302)
            self.send_header('Location', '/index.html')
            self.end_headers()
        else:
            super().do_GET()

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='build')
    ap.add_argument('--port', type=int, default=8080)
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    if not os.path.isdir(root):
        raise SystemExit(f"Missing root '{root}'. Run make first.")
    print(f"Serving {root} on http://localhost:{a.port}")
    httpd = HTTPServer(('localhost', a.port), partial(Handler, directory=root))
    try: httpd.serve_forever()
    except KeyboardInterrupt: print('\nStop')

if __name__ == '__main__':
    main()
