"""PC-only test copy of the shared transfer implementation.

Usage: python -B tools/serve-games.py --port 18765 --host 127.0.0.1
Phone transfers must use the owner-managed shared server in E:/ios-ports.
Shared registration changes belong in docs/공용폴더-반영요청.md.
"""
import argparse
import fnmatch
import json
import os
import sys
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
CONFIG = os.path.join(HERE, 'games.json')


def matches(path, patterns):
    """Glob match with '!pattern' re-including what an earlier pattern excluded."""
    hit = False
    for pat in patterns:
        if pat.startswith('!'):
            if fnmatch.fnmatch(path, pat[1:]):
                hit = False
        elif fnmatch.fnmatch(path, pat):
            hit = True
    return hit


def build_manifest(name, game, lite):
    """The file list and, for each relative path, the file on this PC that is sent for it.
    'overlay' folders (optional, laid out like 'path') replace or add files, e.g. NFS3's
    high-resolution car archives; the game folder itself is never changed."""
    root = game['path']
    include = game.get('include', ['*'])
    exclude = game.get('exclude', []) + (game.get('lite_exclude', []) if lite else [])
    # 'include_manifest' (optional): a JSON file in the game folder whose "after"
    # entries list the only files to send (e.g. RTK5's audited PATCH-MANIFEST.json).
    allowed = None
    if game.get('include_manifest'):
        with open(os.path.join(root, game['include_manifest']), encoding='utf-8') as f:
            allowed = {r['path'].replace('\\', '/').lower() for r in json.load(f)['after']}
    sources = {}
    for dirpath, _, names in os.walk(root):
        for n in names:
            full = os.path.join(dirpath, n)
            rel = os.path.relpath(full, root).replace('\\', '/')
            if allowed is not None and rel.lower() not in allowed:
                continue
            if matches(rel, include) and not matches(rel, exclude):
                sources[rel] = full
    for overlay in game.get('overlay', []):
        for dirpath, _, names in os.walk(overlay):
            for n in names:
                full = os.path.join(dirpath, n)
                rel = os.path.relpath(full, overlay).replace('\\', '/')
                # an overlay file takes the place of the game file with the same name in any case
                same = next((p for p in sources if p.lower() == rel.lower()), rel)
                sources[same] = full
    files = sorted(({'p': p, 's': os.path.getsize(f)} for p, f in sources.items()), key=lambda f: f['p'])
    return {'app': name, 'total': sum(f['s'] for f in files), 'files': files}, sources


class Handler(BaseHTTPRequestHandler):
    games = {}
    manifests = {}
    sources = {}

    def log_message(self, fmt, *args):
        pass  # one line per file would flood the console; progress is shown below

    def send_json(self, obj, code=200):
        body = json.dumps(obj, ensure_ascii=False).encode('utf-8')
        self.send_response(code)
        self.send_header('Content-Type', 'application/json; charset=utf-8')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        parts = urllib.parse.unquote(self.path.split('?', 1)[0]).split('/', 4)
        # ['', 'api', 'manifest'|'file', App, path]
        if self.path == '/' or self.path.startswith('/?'):
            names = ', '.join(sorted(self.games))
            return self.send_json({'apps': sorted(self.games), 'hint': f'Open an app and use PC에서 받기 ({names}).'})
        if len(parts) < 4 or parts[1] != 'api' or parts[3] not in self.games:
            return self.send_json({'error': 'not found'}, 404)
        app = parts[3]
        if parts[2] == 'manifest':
            m = self.manifests[app]
            print(f'  {app}: file list sent ({len(m["files"])} files, {m["total"] / 1e9:.2f} GB) to {self.client_address[0]}')
            return self.send_json(m)
        if parts[2] != 'file' or len(parts) < 5:
            return self.send_json({'error': 'not found'}, 404)
        rel = parts[4]
        full = self.sources[app].get(rel)
        if not full:
            return self.send_json({'error': 'not in manifest'}, 404)
        size = os.path.getsize(full)
        start, end, code = 0, size - 1, 200
        rng = self.headers.get('Range')
        if rng and rng.startswith('bytes='):
            a, _, b = rng[6:].partition('-')
            start = int(a) if a else 0
            end = int(b) if b else size - 1
            code = 206
        self.send_response(code)
        self.send_header('Content-Type', 'application/octet-stream')
        self.send_header('Content-Length', str(end - start + 1))
        if code == 206:
            self.send_header('Content-Range', f'bytes {start}-{end}/{size}')
        self.end_headers()
        with open(full, 'rb') as f:
            f.seek(start)
            left = end - start + 1
            while left > 0:
                chunk = f.read(min(1 << 20, left))
                if not chunk:
                    break
                try:
                    self.wfile.write(chunk)
                except (BrokenPipeError, ConnectionResetError):
                    return
                left -= len(chunk)




def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--port', type=int, required=True, help='PC-only: 18765')
    ap.add_argument('--host', required=True, help='PC-only: 127.0.0.1')
    ap.add_argument('--full', action='store_true', help='also send high-quality videos (MV Player)')
    args = ap.parse_args()
    if args.port != 18765 or args.host != '127.0.0.1':
        ap.error('PC-only test copy requires --port 18765 --host 127.0.0.1')

    with open(CONFIG, encoding='utf-8') as f:
        games = {k: v for k, v in json.load(f).items() if not k.startswith('_')}
    if set(games) != {'NFS4'}:
        ap.error('This local test config must contain only NFS4')
    games = {k: v for k, v in games.items() if os.path.isdir(v['path'])}
    Handler.games = games
    for k, v in games.items():
        Handler.manifests[k], Handler.sources[k] = build_manifest(k, v, lite=not args.full)

    print('NFS4 PC-only loopback test: http://127.0.0.1:18765')
    for k, m in Handler.manifests.items():
        print(f'  {k}: {len(m["files"])} files, {m["total"]} bytes')
    print('Phone transfers use the shared server, not this test copy.')
    sys.stdout.flush()
    ThreadingHTTPServer((args.host, args.port), Handler).serve_forever()


if __name__ == '__main__':
    main()
