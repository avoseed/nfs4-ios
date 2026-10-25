"""Serve game folders to the iOS port apps over Wi-Fi ("PC에서 받기").

The apps ask for a file list, then fetch each file into their own game folder,
skipping files they already have, so an interrupted transfer just resumes.

    python serve-games.py            # MV Player gets low-quality videos only (lite)
    python serve-games.py --full     # everything, including high-quality videos

Protocol (shared by all port apps, see E:\\ios-ports\\README.md):
    GET /api/manifest/<App>       -> {"app", "total", "files": [{"p": path, "s": size}]}
    GET /api/file/<App>/<path>    -> the file (Range supported)
Only folders listed in games.json are served, read-only, on the local network.
"""
import argparse
import fnmatch
import json
import os
import socket
import sys
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
CONFIG = os.path.join(HERE, 'games.json')
if not os.path.isfile(CONFIG):
    CONFIG = os.path.join(HERE, 'games.example.json')


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
    sources = {}
    for dirpath, _, names in os.walk(root):
        for n in names:
            full = os.path.join(dirpath, n)
            rel = os.path.relpath(full, root).replace('\\', '/')
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


def lan_addresses():
    addrs = set()
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(('192.168.0.1', 9))  # no packet is sent; just picks the LAN interface
        addrs.add(s.getsockname()[0])
        s.close()
    except OSError:
        pass
    for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
        ip = info[4][0]
        if not ip.startswith('127.'):
            addrs.add(ip)
    return sorted(addrs)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--port', type=int, default=8765)
    ap.add_argument('--host', default='0.0.0.0', help='127.0.0.1 to test on this PC only')
    ap.add_argument('--full', action='store_true', help='also send high-quality videos (MV Player)')
    args = ap.parse_args()

    with open(CONFIG, encoding='utf-8') as f:
        games = {k: v for k, v in json.load(f).items() if not k.startswith('_')}
    for game in games.values():
        if not os.path.isabs(game['path']):
            game['path'] = os.path.abspath(os.path.join(HERE, game['path']))
    games = {k: v for k, v in games.items() if os.path.isdir(v['path'])}
    Handler.games = games
    for k, v in games.items():
        Handler.manifests[k], Handler.sources[k] = build_manifest(k, v, lite=not args.full)

    print('게임 파일 전송 (PC에서 받기)')
    for k, m in Handler.manifests.items():
        print(f'  {k:12s} {len(m["files"]):6d} files  {m["total"] / 1e9:5.2f} GB')
    print()
    print('아이폰 앱의 "PC에서 받기"에 아래 주소 중 하나를 넣으세요 (같은 와이파이):')
    for ip in lan_addresses():
        print(f'  {ip}:{args.port}')
    print()
    print('다 받으면 이 창에서 Ctrl+C로 끄세요.')
    sys.stdout.flush()
    ThreadingHTTPServer((args.host, args.port), Handler).serve_forever()


if __name__ == '__main__':
    main()
