# Track textures out of the game's own files, as PNGs for redrawing (the way the other ports' redraws are made:
# E:\rtk5-ios\art\redraw\README.md). Nothing in the game folder is changed.
#   py -3 tools/redraw_export.py [--root D:/upscale/NFS4] [--tracks game/data/TRACKS] [TRACK ...]
# -> <root>/input/<TRACK>/<file>_<nnn>_<name>_<w>x<h>.png and input/index.json
#   py -3 tools/redraw_export.py [--root R] --dump <texture dump folder> NAME=KEY ...
# -> <root>/input/UI/<NAME>_<w>x<h>.png: a texture the game builds at run time (the race HUD), see export_dump
# The redraw work lives on D: (the drive with room; E: and F: are short of space), not in the repository.
# A track's TR0.QFS (TRN0: night) and SKY.QFS are RefPack-compressed SHPI ("FSH") picture lists.
import sys, struct, json, pathlib
import numpy as np
from PIL import Image

DEFAULT_ROOT = 'D:/upscale/NFS4'


def refpack(d):
    p = 4 if d[0:2] != b'\x10\xfb' and d[4:6] == b'\x10\xfb' else 0
    if d[p + 1] != 0xFB:
        raise ValueError('not RefPack')
    flags = d[p]
    p += 2
    if flags & 1:
        p += 3
    size = int.from_bytes(d[p:p + 3], 'big')
    p += 3
    out = bytearray()
    n = len(d)
    while p < n:
        c = d[p]
        if c < 0x80:
            b1 = d[p + 1]; p += 2
            lit = c & 3; cnt = ((c & 0x1C) >> 2) + 3; off = ((c & 0x60) << 3) + b1 + 1
        elif c < 0xC0:
            b1, b2 = d[p + 1], d[p + 2]; p += 3
            lit = b1 >> 6; cnt = (c & 0x3F) + 4; off = ((b1 & 0x3F) << 8) + b2 + 1
        elif c < 0xE0:
            b1, b2, b3 = d[p + 1], d[p + 2], d[p + 3]; p += 4
            lit = c & 3; cnt = ((c & 0x0C) << 6) + b3 + 5; off = ((c & 0x10) << 12) + (b1 << 8) + b2 + 1
        elif c < 0xFC:
            lit = ((c & 0x1F) << 2) + 4; p += 1
            out += d[p:p + lit]; p += lit
            continue
        else:
            lit = c & 3; p += 1
            out += d[p:p + lit]
            break
        out += d[p:p + lit]; p += lit
        s = len(out) - off
        for i in range(cnt):
            out.append(out[s + i])
    return bytes(out[:size]) if size else bytes(out)


def palette(d, off):
    """256 RGBA tuples from a palette block, or None."""
    code = d[off] & 0x7F
    n = struct.unpack_from('<H', d, off + 4)[0] or 256
    p = off + 16
    pal = []
    if code == 0x24:                                   # 24-bit RGB
        pal = [(d[p + 3 * i], d[p + 3 * i + 1], d[p + 3 * i + 2], 255) for i in range(n)]
    elif code == 0x22:                                 # 24-bit, 6 bits a channel
        pal = [tuple(min(255, d[p + 3 * i + k] << 2) for k in range(3)) + (255,) for i in range(n)]
    elif code == 0x2A:                                 # 32-bit BGRA
        pal = [(d[p + 4 * i + 2], d[p + 4 * i + 1], d[p + 4 * i], d[p + 4 * i + 3]) for i in range(n)]
    elif code in (0x2D, 0x29):                         # 16-bit: 1555 / 565
        for i in range(n):
            v = struct.unpack_from('<H', d, p + 2 * i)[0]
            if code == 0x2D:
                pal.append((((v >> 10) & 31) * 255 // 31, ((v >> 5) & 31) * 255 // 31, (v & 31) * 255 // 31, 255 if v & 0x8000 else 0))
            else:
                pal.append((((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31, 255))
    else:
        return None
    return pal + [(0, 0, 0, 255)] * (256 - len(pal))


def fsh(d):
    """[(name, code, w, h, RGBA image or None)]"""
    if d[:4] != b'SHPI':
        raise ValueError('not SHPI')
    count = struct.unpack_from('<I', d, 8)[0]
    entries = [(d[16 + 8 * i:20 + 8 * i].decode('latin1'), struct.unpack_from('<I', d, 20 + 8 * i)[0]) for i in range(count)]
    shared = None
    for name, off in entries:
        if name == '!pal' or (d[off] & 0x7F) in (0x24, 0x22, 0x2A, 0x2D, 0x29):
            shared = shared or palette(d, off)
    out = []
    for name, off in entries:
        code = d[off] & 0x7F
        w, h = struct.unpack_from('<HH', d, off + 4)
        img = None
        p = off + 16
        if code == 0x7B and w and h:
            pal = None
            nxt = int.from_bytes(d[off + 1:off + 4], 'little')
            a = off
            while nxt:                                 # attachments: the picture's own palette, if any
                a += nxt
                pal = pal or palette(d, a)
                nxt = int.from_bytes(d[a + 1:a + 4], 'little')
            pal = pal or shared
            if pal:
                img = Image.new('RGBA', (w, h))
                img.putdata([pal[i] for i in d[p:p + w * h]])
        elif code == 0x7D and w and h:
            img = Image.frombytes('RGBA', (w, h), d[p:p + 4 * w * h], 'raw', 'BGRA')
        elif code == 0x7F and w and h:
            img = Image.frombytes('RGB', (w, h), d[p:p + 3 * w * h], 'raw', 'BGR').convert('RGBA')
        elif code == 0x7E and w and h:
            img = Image.new('RGBA', (w, h))
            px = struct.unpack_from('<%dH' % (w * h), d, p)
            img.putdata([(((v >> 10) & 31) * 255 // 31, ((v >> 5) & 31) * 255 // 31, (v & 31) * 255 // 31, 255 if v & 0x8000 else 0) for v in px])
        elif code == 0x78 and w and h:                 # 565 (the front end's screens and car photos)
            img = Image.new('RGBA', (w, h))
            px = struct.unpack_from('<%dH' % (w * h), d, p)
            img.putdata([(((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31, 255) for v in px])
        if code in (0x7B, 0x7D, 0x7F, 0x7E, 0x78, 0x6D):
            out.append((name, code, w, h, img))
    return out


def export_dump(dest, dump, names):
    """Textures the game builds itself (the race HUD is one 256 px picture put together from HUD98.QFS at run time),
    taken from a texture dump (NFS4_DUMP_TEXTURES on the test host) by key: NAME=KEY ... -> input/UI/<NAME>_<w>x<h>.png.
    Each is sent alone on a sheet and comes back at 4x, up to 1024 px (index 'big', 'out_max'); 'soft' marks
    see-through parts with in-between alpha (4444 glows), which the fit step enlarges smoothly instead of hard."""
    path = dest / 'input' / 'index.json'
    index = json.loads(path.read_text(encoding='utf-8')) if path.exists() else {}
    (dest / 'input' / 'UI').mkdir(parents=True, exist_ok=True)
    for spec in names:
        name, key = spec.split('=')
        d = (dump / (key + '.rgba')).read_bytes()
        w, h = struct.unpack_from('<II', d, 4)
        img = Image.frombytes('RGBA', (w, h), d[12:12 + 4 * w * h])
        k = 'UI/%s_%dx%d' % (name, w, h)
        img.save(dest / 'input' / (k + '.png'))
        a = set(np.unique(np.asarray(img.getchannel('A'))).tolist())
        index[k] = {'track': 'UI', 'file': 'dump', 'n': 0, 'name': name, 'code': 0, 'w': w, 'h': h,
                    'alpha': min(a) < 255, 'soft': bool(a - {0, 255}), 'big': True, 'out_max': 1024, 'key': key}
        print('%s: %dx%d, alpha %s' % (k, w, h, 'soft' if index[k]['soft'] else index[k]['alpha']))
    path.write_text(json.dumps(index, indent=1), encoding='utf-8')


def main():
    args = sys.argv[1:]
    def option(flag, default):
        if flag in args:
            i = args.index(flag)
            value = args[i + 1]
            del args[i:i + 2]
            return value
        return default
    dest = pathlib.Path(option('--root', DEFAULT_ROOT))
    tracks = pathlib.Path(option('--tracks', str(pathlib.Path(__file__).resolve().parents[1] / 'game' / 'data' / 'TRACKS')))
    dump = option('--dump', None)
    if dump:
        return export_dump(dest, pathlib.Path(dump), args)
    want = [t.upper() for t in args]
    index = {}
    for track in sorted(p for p in tracks.iterdir() if p.is_dir()):
        if want and track.name.upper() not in want:
            continue
        for f in sorted(track.glob('*.QFS')):
            out = dest / 'input' / track.name
            out.mkdir(parents=True, exist_ok=True)
            done = skipped = 0
            for n, (name, code, w, h, img) in enumerate(fsh(refpack(f.read_bytes()))):
                if img is None:
                    skipped += 1
                    continue
                safe = ''.join(c if c.isalnum() else '_' for c in name)
                key = '%s_%03d_%s_%dx%d' % (f.stem, n, safe, w, h)
                img.save(out / (key + '.png'))
                alpha = img.getchannel('A').getextrema()
                index['%s/%s' % (track.name, key)] = {'track': track.name, 'file': f.name, 'n': n, 'name': name, 'code': code,
                                                    'w': w, 'h': h, 'alpha': alpha[0] < 255}
                done += 1
            print('%-9s %-9s %d pictures, %d not decoded' % (track.name, f.name, done, skipped))
    (dest / 'input').mkdir(parents=True, exist_ok=True)
    path = dest / 'input' / 'index.json'
    old = json.loads(path.read_text(encoding='utf-8')) if path.exists() else {}
    old.update(index)
    path.write_text(json.dumps(old, indent=1), encoding='utf-8')


if __name__ == '__main__':
    main()
