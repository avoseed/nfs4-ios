# The redraw pack in ASTC 4x4, the compressed form the iPhone GPU samples directly (recomp/runtime/hd_textures.c).
#   py -3 -B tools/hd_astc.py [--in D:/upscale/NFS4/pack] [--out D:/upscale/NFS4/pack-astc] [--decoded DIR]
#       [--quality medium|thorough] [--astcenc EXE] [KEY ...]
#   every <in>/hdtex/<key>.rgba (HDT1, tools/hd_texture_pack.py and menu_hd.py) -> <out>/hdtex/<key>.astc
#   --decoded DIR also writes what the GPU will see, level 0 decoded back to HDT1 (<DIR>/hdtex/<key>.rgba), for
#   running the Windows host (Direct3D 11 has no ASTC) with exactly the compressed pictures.
# File: "HDT2", width, height, levels, block size (0x0404), u32 little-endian, then every mip level from the full size
# down to 1x1, each ceil(w/4) x ceil(h/4) blocks of 16 bytes row by row. Mips are 2x2 averages, as Metal's
# generateMipmaps makes them for the uncompressed pack. 1 byte a pixel at level 0 instead of 4, so a whole track's
# redraw (GERMANY, the largest) sits well inside the 64 MiB texture budget of hle_glide.c.
# Encoder: ARM astcenc 5.7.0 (official release, checksum in F:/FF7-iOS/tools/astcenc/provenance.json). Textures are
# laid out side by side on 4096-wide sheets on 4-pixel steps, so one astcenc run encodes hundreds of them; a block
# never crosses two pictures, so each one's blocks are cut out unchanged.
import sys, struct, pathlib, subprocess, tempfile, json, time
import numpy as np
from PIL import Image

ASTCENC = 'F:/FF7-iOS/tools/astcenc/bin/astcenc-avx2.exe'
SHEET_W, SHEET_H = 4096, 4096


def read_hdt1(path):
    b = path.read_bytes()
    magic, w, h = struct.unpack_from('<4sII', b)
    assert magic == b'HDT1', path
    return np.frombuffer(b, np.uint8, w * h * 4, 12).reshape(h, w, 4)


def levels_of(a):
    out = [a]
    while a.shape[0] > 1 or a.shape[1] > 1:
        h, w = a.shape[:2]
        nh, nw = max(1, h // 2), max(1, w // 2)
        f = a[:nh * (h // nh), :nw * (w // nw)].astype(np.uint32)
        f = f.reshape(nh, h // nh, nw, w // nw, 4).mean(axis=(1, 3))
        a = (f + 0.5).astype(np.uint8)
        out.append(a)
    return out


def pad4(a):
    h, w = a.shape[:2]
    return np.pad(a, ((0, -h % 4), (0, -w % 4), (0, 0)), mode='edge')


class Sheet:
    def __init__(self):
        self.img = np.zeros((SHEET_H, SHEET_W, 4), np.uint8)
        self.x = self.y = self.row_h = 0
        self.used_h = 0
        self.items = []                     # (key, level, x, y, w, h)

    def place(self, key, lvl, a):
        p = pad4(a)
        h, w = p.shape[:2]
        if self.x + w > SHEET_W:
            self.x, self.y, self.row_h = 0, self.y + self.row_h, 0
        if self.y + h > SHEET_H:
            return False
        self.img[self.y:self.y + h, self.x:self.x + w] = p
        self.items.append((key, lvl, self.x, self.y, a.shape[1], a.shape[0]))
        self.x += w
        self.row_h = max(self.row_h, h)
        self.used_h = max(self.used_h, self.y + h)
        return True


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.exit('astcenc failed: %s\n%s%s' % (' '.join(cmd), r.stdout, r.stderr))


def encode_sheet(sheet, tmp, exe, quality, want_decoded):
    h = sheet.used_h
    src, comp, dec = tmp / 'sheet.png', tmp / 'sheet.astc', tmp / 'sheet_dec.png'
    Image.fromarray(sheet.img[:h], 'RGBA').save(src, compress_level=1)
    run([exe, '-cl', str(src), str(comp), '4x4', '-' + quality, '-silent'])
    raw = comp.read_bytes()
    assert raw[:4] == b'\x13\xab\xa1\x5c' and raw[4:6] == b'\x04\x04'
    bx = (SHEET_W + 3) // 4
    blocks = np.frombuffer(raw, np.uint8, offset=16).reshape(-1, bx, 16)
    decoded = None
    if want_decoded:
        run([exe, '-dl', str(comp), str(dec), '-silent'])
        decoded = np.asarray(Image.open(dec).convert('RGBA'))
    parts = {}
    for key, lvl, x, y, w, hh in sheet.items:
        b = blocks[y // 4:(y + hh + 3) // 4, x // 4:(x + w + 3) // 4]
        parts[(key, lvl)] = (b.tobytes(), None if decoded is None or lvl else decoded[y:y + hh, x:x + w].copy())
    return parts


def main():
    args = sys.argv[1:]
    def option(flag, default):
        if flag in args:
            i = args.index(flag); v = args[i + 1]; del args[i:i + 2]; return v
        return default
    src = pathlib.Path(option('--in', 'D:/upscale/NFS4/pack')) / 'hdtex'
    out = pathlib.Path(option('--out', 'D:/upscale/NFS4/pack-astc')) / 'hdtex'
    decoded_dir = option('--decoded', None)
    quality = option('--quality', 'medium')
    exe = option('--astcenc', ASTCENC)
    out.mkdir(parents=True, exist_ok=True)
    if decoded_dir:
        decoded_dir = pathlib.Path(decoded_dir) / 'hdtex'
        decoded_dir.mkdir(parents=True, exist_ok=True)
    keys = args or sorted(p.stem for p in src.glob('*.rgba'))
    t0 = time.time()
    stats = {}                               # key -> (psnr, alpha flips)
    total_in = total_out = 0
    with tempfile.TemporaryDirectory() as td:
        tmp = pathlib.Path(td)
        pending = []                         # keys whose levels are on the current sheet
        sheet = Sheet()
        got = {}
        originals = {}

        def flush():
            nonlocal sheet, pending, total_out
            if not sheet.items:
                return
            got.update(encode_sheet(sheet, tmp, exe, quality, decoded_dir is not None))
            for key in pending:
                a = originals.pop(key)
                h, w = a.shape[:2]
                n = len(levels_of_count(w, h))
                data = b''.join(got.pop((key, l))[0] if l else got[(key, 0)][0] for l in range(n))
                dst = out / (key + '.astc')
                dst.write_bytes(struct.pack('<4sIIII', b'HDT2', w, h, n, 0x0404) + data)
                total_out += dst.stat().st_size
                dec = got.pop((key, 0))[1]
                if dec is not None:
                    (decoded_dir / (key + '.rgba')).write_bytes(struct.pack('<4sII', b'HDT1', w, h) + dec.tobytes())
                    d = dec.astype(np.int32) - a.astype(np.int32)
                    mse = float(np.mean(d[..., :3] ** 2))
                    flips = int(np.count_nonzero((a[..., 3] == 0) != (dec[..., 3] == 0)))
                    stats[key] = (99.0 if mse == 0 else 10 * np.log10(255 ** 2 / mse), flips, int(np.abs(d).max()))
            sheet, pending = Sheet(), []

        for i, key in enumerate(keys):
            a = read_hdt1(src / (key + '.rgba'))
            total_in += a.nbytes + 12
            lv = levels_of(a)
            for attempt in range(2):
                saved = (sheet.x, sheet.y, sheet.row_h, sheet.used_h, len(sheet.items))
                if all(sheet.place(key, n, l) for n, l in enumerate(lv)):
                    break
                sheet.x, sheet.y, sheet.row_h, sheet.used_h, cnt = saved      # (pixels left behind are overwritten)
                del sheet.items[cnt:]
                flush()
            else:
                sys.exit('%s does not fit a sheet' % key)
            originals[key] = a
            pending.append(key)
            if (i + 1) % 500 == 0:
                print('%d/%d, %.0f s' % (i + 1, len(keys), time.time() - t0), flush=True)
        flush()
    print('%s: %d textures, %.1f MB -> %.1f MB (with mips), %.0f s' % (out, len(keys), total_in / 1e6, total_out / 1e6, time.time() - t0))
    if stats:
        report = out.parent.parent / (out.parent.name + '-report.json')     # beside, not inside, the served folder
        report.write_text(json.dumps(stats, indent=0), encoding='utf-8')
        ps = sorted(stats.items(), key=lambda kv: kv[1][0])
        print('PSNR (RGB, level 0): median %.1f dB, worst %s' % (ps[len(ps) // 2][1][0], ', '.join('%s %.1f' % (k, v[0]) for k, v in ps[:5])))
        fl = sorted(stats.items(), key=lambda kv: -kv[1][1])
        print('alpha 0 <-> not 0 changes: %d textures, most %s' % (sum(1 for v in stats.values() if v[1]), ', '.join('%s %d' % (k, v[1]) for k, v in fl[:5])))


def levels_of_count(w, h):
    n = [(w, h)]
    while w > 1 or h > 1:
        w, h = max(1, w // 2), max(1, h // 2)
        n.append((w, h))
    return n


if __name__ == '__main__':
    main()
