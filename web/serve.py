#!/usr/bin/env python3
"""Local test server for the web build.

Serves this folder on http://localhost:8080 with the headers of _headers
(the website's: COOP/COEP for SharedArrayBuffer, the Content-Security-Policy
and the other hardening headers), so local tests match the real host.
"""
import http.server, os, sys, webbrowser

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8080

class Handler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {**http.server.SimpleHTTPRequestHandler.extensions_map,
                      '.wasm': 'application/wasm', '.js': 'text/javascript'}
    def end_headers(self):
        page = self.path.split('?')[0].rsplit('/', 1)[-1]
        for k, v in SITE_HEADERS:
            # development pages (not part of site/) keep their inline scripts
            if k.lower() == 'content-security-policy' and page in DEV_PAGES:
                continue
            self.send_header(k, v)
        self.send_header('Cache-Control', 'no-cache')
        super().end_headers()


DEV_PAGES = {'bench.html', 'hdtest.html'}


def site_headers():
    """The '/*' block of _headers (what the real host sends), so that local
    tests run under the same Content-Security-Policy as the website."""
    out, cur = [], None
    try:
        for line in open('_headers', encoding='utf-8'):
            if not line.strip() or line.lstrip().startswith('#'):
                continue
            if not line[0].isspace():
                cur = line.strip()
            elif cur == '/*' and ':' in line:
                k, v = line.strip().split(':', 1)
                out.append((k.strip(), v.strip()))
    except OSError:
        pass
    keys = {k for k, _ in out}
    for k, v in (('Cross-Origin-Opener-Policy', 'same-origin'),
                 ('Cross-Origin-Embedder-Policy', 'require-corp')):
        if k not in keys:
            out.append((k, v))
    return out

os.chdir(os.path.dirname(os.path.abspath(__file__)))
SITE_HEADERS = site_headers()
print(f'Serving {os.getcwd()} on http://localhost:{PORT}  (Ctrl+C to stop)')
if '--no-browser' not in sys.argv:
    webbrowser.open(f'http://localhost:{PORT}/')
http.server.ThreadingHTTPServer(('127.0.0.1', PORT), Handler).serve_forever()
