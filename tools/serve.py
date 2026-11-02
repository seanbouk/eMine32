"""Dev server for the web build: serves web/out with caching disabled, so a rebuild is always picked up.
usage: python tools/serve.py [port]   (default 8032)
"""
import functools, http.server, os, sys

class NoCache(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header('Cache-Control', 'no-store, must-revalidate')
        self.send_header('Expires', '0')
        super().end_headers()
    def log_message(self, *a):
        pass

port = int(sys.argv[1]) if len(sys.argv) > 1 else 8032
root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'web', 'out')
handler = functools.partial(NoCache, directory=root)
print('serving %s at http://localhost:%d/eduke32.html (no caching)' % (os.path.normpath(root), port))
http.server.ThreadingHTTPServer(('', port), handler).serve_forever()
