#!/usr/bin/env python3
"""Minimal HTTP server with COOP/COEP headers for OPFS experiments."""
import http.server
import sys
import os

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 18090
DIRECTORY = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'build')

class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=DIRECTORY, **kwargs)

    def end_headers(self):
        self.send_header('Cross-Origin-Opener-Policy', 'same-origin')
        self.send_header('Cross-Origin-Embedder-Policy', 'require-corp')
        super().end_headers()

print(f'Serving {DIRECTORY} on http://localhost:{PORT}')
http.server.HTTPServer(('', PORT), Handler).serve_forever()
