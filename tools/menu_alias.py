# Menu tiles that show a picture already redrawn, under another key (tools/menu_hd.py).
#   py -3 -B tools/menu_alias.py <dump dir> [<dump dir> ...] [--root D:/upscale/NFS4] [--dry]
# The spinning globe is drawn over each screen's background, so the same frame reaches the GPU with different pixels
# where it is see-through (the key covers them too) and every screen needs its own copy. For every dumped texture
# that is in neither pack (<root>/pack/hdtex), this finds a fitted menu picture of the same size (input/MENU/<name>.png
# with output/MENU/<name>.png) whose pixels are the same wherever the dumped one is seen (alpha > 0), and that is seen
# at least there. Such a texture is added as a picture of its own (menu_hd.py add) with the fitted picture's redraw,
# cut to the dumped one's see-through shape. Anything else is listed, for a redraw bundle (menu_hd.py add + bundle).
# Then: menu_hd.py pack, hd_astc.py <keys>.
import sys, json, struct, pathlib, subprocess
import numpy as np
from PIL import Image

DEFAULT_ROOT = 'D:/upscale/NFS4'


def read_dump(p):
    b = p.read_bytes()
    w, h = struct.unpack_from('<II', b, 4)
    return np.frombuffer(b, np.uint8, w * h * 4, 12).reshape(h, w, 4)


def main():
    args = sys.argv[1:]
    root = pathlib.Path(DEFAULT_ROOT)
    if '--root' in args:
        i = args.index('--root'); root = pathlib.Path(args[i + 1]); del args[i:i + 2]
    dry = '--dry' in args
    if dry:
        args.remove('--dry')
    packed = {p.stem for p in (root / 'pack' / 'hdtex').glob('*.rgba')}
    menu = json.loads((root / 'input' / 'menu.json').read_text(encoding='utf-8'))
    by_key = {e['tiles'][0][0]: name for name, e in menu.items() if len(e['tiles']) == 1}
    fitted = {}                                   # (w, h) -> [(name, rgba)]
    for name, e in menu.items():
        if (root / 'output' / (name + '.png')).exists() and len(e['tiles']) == 1:
            a = np.asarray(Image.open(root / 'input' / (name + '.png')).convert('RGBA'))
            fitted.setdefault((a.shape[1], a.shape[0]), []).append((name, a))
    seen, added, left = set(), [], []
    for d in args:
        for p in sorted(pathlib.Path(d).glob('*.rgba')):
            key = p.stem
            if key in packed or key in seen:
                continue
            seen.add(key)
            a = read_dump(p)
            h, w = a.shape[:2]
            if w != h or w < 64:                  # menu globe frames and icons; 32 px tiles are fonts and bars
                continue
            m = a[..., 3] > 0
            best, diff = None, None
            for name, b in fitted.get((w, h), []):
                if (m & ~(b[..., 3] > 0)).any():
                    continue
                dm = m & (a[..., :3] != b[..., :3]).any(-1)
                if diff is None or dm.sum() < diff.sum():
                    best, diff = name, dm
            if not best or diff.sum() > max(4, m.sum() // 100):
                left.append('%s %dx%d' % (key, w, h))
                continue
            added.append((key, best, int(diff.sum())))
            if dry:
                continue
            new_name = by_key.get(key)                 # a picture of its own already (one held back from a redraw)
            if not new_name:
                r = subprocess.run([sys.executable, '-B', str(pathlib.Path(__file__).with_name('menu_hd.py')), 'add',
                                    d, key, '--root', str(root)], capture_output=True, text=True)
                if r.returncode:
                    sys.exit(r.stdout + r.stderr)
                new_name = r.stdout.split()[-1]
            o = np.asarray(Image.open(root / 'output' / (best + '.png')).convert('RGBA')).copy()
            s = o.shape[0] // h
            up = lambda x: np.kron(x, np.ones((s, s) + (1,) * (x.ndim - 2), np.uint8))
            o[..., 3] = np.where(up(m.astype(np.uint8)) > 0, o[..., 3], 0)
            o[up(diff.astype(np.uint8)) > 0] = up(a)[up(diff.astype(np.uint8)) > 0]   # the few pixels that differ
            Image.fromarray(o, 'RGBA').save(root / 'output' / (new_name + '.png'))
    for key, best, n in added:
        print('alias %s <- %s%s' % (key, best, ', %d pixels from the original' % n if n else ''))
    for x in left:
        print('no match %s' % x)
    print('%d aliased, %d without a redrawn twin' % (len(added), len(left)))


if __name__ == '__main__':
    main()
