# moved from tests/test_server.py unchanged
#!/usr/bin/env python3
import http.server, socketserver, sys, os, argparse

class Handler(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        # COOP/COEP for SharedArrayBuffer support
        self.send_header('Cross-Origin-Opener-Policy', 'same-origin')
        self.send_header('Cross-Origin-Embedder-Policy', 'require-corp')
        super().end_headers()

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='build')
    ap.add_argument('--port', type=int, default=18080)
    args = ap.parse_args()
    os.chdir(args.root)
    socketserver.TCPServer.allow_reuse_address = True
    with socketserver.TCPServer(('', args.port), Handler) as httpd:
        print(f"Test server running on port {args.port} serving {args.root}")
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            pass

if __name__ == '__main__':
    main()
