"""File server for moving builds to the Steam Deck and logs back, over the local network (no cable).

    python tools/deck_server.py <folder to share> [--uploads <folder>] [--bind 192.168.1.73] [--port 8000]

On the Deck, open http://<this PC>:<port>/ in a browser: the shared folder can be browsed and downloaded, and the
"Upload files" page sends files (last.log, screenshots) to the uploads folder on this PC. LAN only: anyone on the
network who can reach the port can read the shared folder and upload, so stop it when done.
"""
import argparse, html, http.server, io, os, re, shutil

UPLOAD_PAGE = """<!doctype html><meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1">
<title>Upload to PC</title>
<style>body{font:18px sans-serif;margin:24px;max-width:640px}input,button{font-size:18px;margin:8px 0}#log div{margin:4px 0}</style>
<h1>Upload files to the PC</h1>
<p>Files go to <code>%s</code>.</p>
<input type=file id=f multiple><br><button id=go>Upload</button> <a href="/">Back to files</a>
<div id=log></div>
<script>
const log = m => { const d = document.createElement('div'); d.textContent = m; document.getElementById('log').prepend(d); };
document.getElementById('go').onclick = async () => {
  for (const f of document.getElementById('f').files) {
    log('Sending ' + f.name + ' (' + (f.size / 1048576).toFixed(1) + ' MiB)...');
    try {
      const r = await fetch('/upload/' + encodeURIComponent(f.name), {method: 'PUT', body: f});
      log(r.ok ? 'Saved as ' + await r.text() : 'Failed: ' + r.status + ' ' + await r.text());
    } catch (e) { log('Failed: ' + e); }
  }
};
</script>"""


def make_handler(share, uploads):
    class H(http.server.SimpleHTTPRequestHandler):
        def __init__(self, *a, **k): super().__init__(*a, directory=share, **k)

        LINK = b'<p><a href="/upload"><b>Upload files to the PC</b></a></p>'
        listing = False

        def send_response_only(self, code, message=None):
            self.code_sent = code; super().send_response_only(code, message)

        def send_header(self, k, v):   # the listing's length is sent before its page: count the added link
            if self.listing and self.code_sent == 200 and k == 'Content-Length': v = str(int(v) + len(self.LINK))
            super().send_header(k, v)

        def list_directory(self, path):   # the normal listing with an upload link on top
            self.listing = True
            try: f = super().list_directory(path)
            finally: self.listing = False
            if f is None: return None
            page = f.read()
            i = page.find(b'<hr>')
            return io.BytesIO(page[:i] + self.LINK + page[i:])

        def do_GET(self):
            if self.path.rstrip('/') == '/upload':
                body = (UPLOAD_PAGE % html.escape(uploads)).encode()
                self.send_response(200); self.send_header('Content-Type', 'text/html; charset=utf-8')
                self.send_header('Content-Length', str(len(body))); self.end_headers(); self.wfile.write(body)
                return
            super().do_GET()

        def do_PUT(self):
            if not self.path.startswith('/upload/'): return self.send_error(404)
            from urllib.parse import unquote
            name = re.sub(r'[^\w.\- ]', '_', os.path.basename(unquote(self.path[len('/upload/'):]))).strip(' .') or 'upload'
            n = int(self.headers.get('Content-Length') or -1)
            if n < 0: return self.send_error(411, 'Content-Length required')
            base, ext = os.path.splitext(name); dest = os.path.join(uploads, name); k = 1
            while os.path.exists(dest): dest = os.path.join(uploads, f'{base} ({k}){ext}'); k += 1
            tmp = dest + '.part'
            with open(tmp, 'wb') as out:
                left = n
                while left:
                    b = self.rfile.read(min(left, 1 << 20))
                    if not b: break
                    out.write(b); left -= len(b)
            if left: os.remove(tmp); return self.send_error(400, 'upload cut short')
            os.replace(tmp, dest)
            self.log_message('uploaded %s (%d bytes)', dest, n)
            msg = os.path.basename(dest).encode()
            self.send_response(200); self.send_header('Content-Type', 'text/plain; charset=utf-8')
            self.send_header('Content-Length', str(len(msg))); self.end_headers(); self.wfile.write(msg)
    return H


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('share'); ap.add_argument('--uploads', default=os.path.join(os.path.expanduser('~'), 'Documents', 'deck-uploads'))
    ap.add_argument('--bind', default='0.0.0.0'); ap.add_argument('--port', type=int, default=8000)
    a = ap.parse_args()
    share, uploads = os.path.abspath(a.share), os.path.abspath(a.uploads)
    os.makedirs(uploads, exist_ok=True)
    s = http.server.ThreadingHTTPServer((a.bind, a.port), make_handler(share, uploads))
    print(f'sharing {share}\nuploads to {uploads}\nopen http://{a.bind}:{a.port}/ on the Deck (Ctrl+C stops)', flush=True)
    s.serve_forever()


if __name__ == '__main__':
    main()
