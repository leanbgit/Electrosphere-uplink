#!/usr/bin/env python3
"""Local test server for the web build.

Serves this folder on http://localhost:8080 with the two headers browsers
require before they allow SharedArrayBuffer (used for audio, input and pacing):
  Cross-Origin-Opener-Policy: same-origin
  Cross-Origin-Embedder-Policy: require-corp
"""
import http.server, os, sys, webbrowser

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8080

class Handler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {**http.server.SimpleHTTPRequestHandler.extensions_map,
                      '.wasm': 'application/wasm', '.js': 'text/javascript'}
    def end_headers(self):
        self.send_header('Cross-Origin-Opener-Policy', 'same-origin')
        self.send_header('Cross-Origin-Embedder-Policy', 'require-corp')
        self.send_header('Cache-Control', 'no-cache')
        super().end_headers()

os.chdir(os.path.dirname(os.path.abspath(__file__)))
print(f'Serving {os.getcwd()} on http://localhost:{PORT}  (Ctrl+C to stop)')
if '--no-browser' not in sys.argv:
    webbrowser.open(f'http://localhost:{PORT}/')
http.server.ThreadingHTTPServer(('127.0.0.1', PORT), Handler).serve_forever()
