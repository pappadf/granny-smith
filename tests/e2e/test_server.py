#!/usr/bin/env python3
# The e2e suites' static server: COOP/COEP for SharedArrayBuffer, no caching.
import http.server, os, argparse

class Handler(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        # Every spec must see the build under test, never a cached copy of
        # the previous one (the same headers scripts/dev_server.py sends).
        self.send_header('Cache-Control', 'no-cache, no-store, must-revalidate, max-age=0')
        self.send_header('Pragma', 'no-cache')
        self.send_header('Expires', '0')
        # Required for SharedArrayBuffer (pthreads) and OPFS SyncAccessHandle
        self.send_header('Cross-Origin-Opener-Policy', 'same-origin')
        self.send_header('Cross-Origin-Embedder-Policy', 'require-corp')
        super().end_headers()

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='build')
    ap.add_argument('--port', type=int, default=18080)
    args = ap.parse_args()
    os.chdir(args.root)
    # One thread per connection: a page opens several module and worker
    # fetches at once and a pthread worker can hold a connection while it
    # blocks; a single-threaded server stalls every other request behind it
    # (scripts/dev_server.py has the same note).
    with http.server.ThreadingHTTPServer(('', args.port), Handler) as httpd:
        httpd.daemon_threads = True
        print(f"Test server running on port {args.port} serving {args.root}")
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            pass

if __name__ == '__main__':
    main()
