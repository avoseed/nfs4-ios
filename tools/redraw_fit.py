# Received redraw sheets back onto the textures they came from (the fit step of the other ports' redraws,
# E:\rtk5-ios\tools\redraw.py). Nothing is sent anywhere and the game folder is not touched.
#   py -3 tools/redraw_fit.py [--root D:/upscale/NFS4] N [N ...]
# For every box of redraw<N>/boxes.json whose sheet is in received/:
#   - the answer is scaled back to the sheet's canvas (models answer at their own size, e.g. 1254 for 1024),
#   - the box is found again within a few pixels (a model moves things a little) and cut out,
#   - scaled to the output size: 4x, at most 256 px a side (the size Voodoo-era games keep; the HUD, 'out_max', 1024),
#   - the broad colour is taken from the original (a model drifts in brightness), the detail from the answer,
#   - the see-through shape comes from the original's alpha (hard edged, as the game's 1-bit alpha; enlarged smoothly
#     for a 'soft' picture's 4444 glows and for any texture with in-between alpha: clouds, light rays, glass), and any magenta the model left inside it is replaced by the enlarged original,
#   - a thin band at each edge fades into the enlarged original taken with wrap-around, so a texture that tiles
#     on the track still meets itself without a seam.
# -> <root>/output/<TRACK>/<key>.png (RGBA) for the pack (tools/hd_texture_pack.py), and
#    redraw<N>/compare/<key>.png: original enlarged | result, for looking at.
import sys, json, pathlib
import numpy as np
from PIL import Image, ImageFilter

DEFAULT_ROOT = 'D:/upscale/NFS4'
OUT_SCALE, OUT_MAX = 4, 256
SEARCH = 12            # canvas pixels the box is searched around its place
MATCH_SIGMA = 1.2      # original pixels: what is "broad colour"
GREY = 128


def magenta(a):
    """mask of pixels that read as the flat key colour (or a model's smudge of it)"""
    r, g, b = a[..., 0].astype(np.int32), a[..., 1].astype(np.int32), a[..., 2].astype(np.int32)
    return (r > 150) & (b > 150) & (r - g > 90) & (b - g > 90)


def best_shift(ref, got, mask, r):
    """offset (dx, dy) within r of got (bigger by 2r each way) that matches ref best inside mask"""
    h, w = ref.shape
    best = None
    for dy in range(-r, r + 1):
        for dx in range(-r, r + 1):
            part = got[r + dy:r + dy + h, r + dx:r + dx + w]
            d = float(np.abs(part - ref)[mask].mean()) if mask.any() else 0.0
            if best is None or d < best[0]:
                best = (d, dx, dy)
    return best


def gauss(a, sigma):
    """an HxW float array blurred (edges wrap, as a tiling texture's do)"""
    r = max(1, int(sigma * 3 + 0.5))
    k = np.exp(-0.5 * (np.arange(-r, r + 1) / sigma) ** 2); k /= k.sum()
    out = sum(np.roll(a, i, 1) * w for i, w in zip(range(-r, r + 1), k))
    return sum(np.roll(out, i, 0) * w for i, w in zip(range(-r, r + 1), k))


def blur_to(img_f, mask, size, sigma):
    """broad colour of an HxWx3 float array at size (w, h), taken from the pixels in mask only (what is under the
    see-through parts is no colour of the picture's and would tint it)"""
    m = mask.astype(np.float32)
    wsum = gauss(m, sigma)
    ch = [gauss(img_f[..., c] * m, sigma) / np.maximum(wsum, 1e-3) for c in range(3)]
    a = np.stack(ch, 2)
    a[wsum < 1e-3] = img_f[mask].mean(0) if mask.any() else 0
    return np.stack([np.asarray(Image.fromarray(np.ascontiguousarray(a[..., c], np.float32), 'F').resize(size, Image.BICUBIC))
                     for c in range(3)], 2)


def wrap_enlarge(orig_rgb, size):
    """the original enlarged smoothly with wrap-around at the edges (as the track samples a tiling texture)"""
    w, h = orig_rgb.size
    a = np.asarray(orig_rgb)
    big = Image.fromarray(np.tile(a, (3, 3, 1)), 'RGB').resize((size[0] * 3, size[1] * 3), Image.BICUBIC)
    return np.asarray(big.crop((size[0], size[1], size[0] * 2, size[1] * 2)), np.float32)


def fill_under(rgb, opaque, passes=64):
    """colour under the see-through parts taken from the nearest opaque pixels, so filtering brings no fringe"""
    rgb, have = rgb.copy(), opaque.copy()
    if not have.any():
        return rgb
    for _ in range(passes):
        if have.all():
            break
        acc = np.zeros_like(rgb); n = np.zeros(have.shape, np.float32)
        for dy, dx in ((0, 1), (0, -1), (1, 0), (-1, 0)):
            sh = np.roll(have, (dy, dx), (0, 1)); sc = np.roll(rgb, (dy, dx), (0, 1))
            acc += sc * sh[..., None]; n += sh
        new = ~have & (n > 0)
        rgb[new] = acc[new] / n[new][:, None]
        have |= new
    rgb[~have] = rgb[have].mean(0)
    return rgb


def is_soft(b, orig):
    """in-between alpha over more than 2% of the texels: clouds, light rays, shore foam, glass panes (UK bus stop
    and phone box: alpha 83-96). Their see-through amount is the picture, so it must not be cut to 0/255."""
    if b.get('soft'):
        return True
    a = np.asarray(orig.getchannel('A'))
    return bool(((a > 0) & (a < 255)).mean() > 0.02)


def fit_one(sheet, b, orig):
    x, y, w, h = b['box']
    soft = is_soft(b, orig)
    vis = 8 if soft else 128      # the alpha from which a texel's colour counts (a faint cloud is still its colour)
    # the box as sent: the original enlarged the same way, for finding it again
    sent = np.asarray(orig.convert('RGB').resize((w, h), Image.BICUBIC).convert('L'), np.float32)
    # found again by what the model saw: the sheet shows texels under alpha 128 as magenta
    amask = np.asarray(orig.getchannel('A').resize((w, h), Image.NEAREST)) >= 128
    r = b.get('search', SEARCH)   # a box may set its own (0: a mostly see-through menu piece, nothing to find it by)
    region = sheet.crop((x - r, y - r, x + w + r, y + h + r))
    reg = np.asarray(region.convert('L'), np.float32)
    keyed = magenta(np.asarray(region))
    reg_l = reg.copy(); reg_l[keyed] = 0
    ref = sent.copy(); ref[~amask] = 0
    # nothing the model saw (a faint cloud is all under 128): left in place
    d, dx, dy = best_shift(ref, reg_l, amask, r) if amask.any() else (0.0, 0, 0)
    d0 = float(np.abs(reg_l[r:r + h, r:r + w] - ref)[amask].mean()) if amask.any() else 0.0
    cut = region.crop((r + dx, r + dy, r + dx + w, r + dy + h))

    k = min(OUT_SCALE, b.get('out_max', OUT_MAX) / max(orig.size))
    size = (round(orig.width * k), round(orig.height * k))
    out = np.asarray(cut.resize(size, Image.LANCZOS), np.float32)
    big = wrap_enlarge(orig.convert('RGB'), size)
    opaque = np.asarray(orig.getchannel('A').resize(size, Image.NEAREST)) >= vis

    # magenta left inside the picture: the enlarged original there
    bad = magenta(out) & opaque
    # and the model's blend of the key into the picture's rim: near the see-through parts, a pixel much pinker
    # than the original there (red and blue both well over green, more than the original has)
    near = ~opaque
    for _ in range(max(2, round(k * 0.75))):
        near = near | np.roll(near, 1, 0) | np.roll(near, -1, 0) | np.roll(near, 1, 1) | np.roll(near, -1, 1)
    pink = lambda a: np.minimum(a[..., 0] - a[..., 1], a[..., 2] - a[..., 1])
    bad |= opaque & near & (pink(out) - pink(big) > 30)
    out[bad] = big[bad]
    # broad colour from the original, detail from the answer (opaque parts only take part)
    o = np.asarray(orig.convert('RGB'), np.float32)
    small_out = np.asarray(Image.fromarray(np.clip(out + .5, 0, 255).astype(np.uint8), 'RGB').resize(orig.size, Image.BOX), np.float32)
    sigma = MATCH_SIGMA
    keep = np.asarray(orig.getchannel('A')) >= vis
    out = out + blur_to(o, keep, size, sigma) - blur_to(small_out, keep, size, sigma)
    # edges fade into the wrapped original so a tiling texture meets itself
    # (not the HUD: a sheet of parts the game cuts out by coordinates, never tiled)
    band = 0 if 'out_max' in b else max(3, size[0] // 32)
    yy, xx = np.mgrid[0:size[1], 0:size[0]]
    edge = np.minimum(np.minimum(xx, size[0] - 1 - xx), np.minimum(yy, size[1] - 1 - yy)).astype(np.float32)
    t = np.clip((edge + 0.5) / band, 0, 1)[..., None] if band else 1
    out = out * t + big * (1 - t)
    out = fill_under(np.clip(out, 0, 255), opaque)
    alpha = (opaque * 255).astype(np.uint8) if b.get('alpha') else np.full(opaque.shape, 255, np.uint8)
    if soft:
        # in-between alpha (4444 glows, clouds, glass): enlarged smoothly there; elsewhere a smooth but firm edge
        a0 = orig.getchannel('A')
        smooth = np.asarray(a0.resize(size, Image.BICUBIC), np.float32)
        partial = np.asarray(a0.point(lambda v: 255 if 0 < v < 255 else 0).filter(ImageFilter.MaxFilter(3)).resize(size, Image.NEAREST)) > 0
        firm = np.clip((smooth - 128) * 3 + 128, 0, 255)
        alpha = np.clip(np.where(partial, smooth, firm) + .5, 0, 255).astype(np.uint8)
    res = Image.fromarray(np.dstack([np.clip(out + .5, 0, 255).astype(np.uint8), alpha]), 'RGBA')
    flat = (np.asarray(orig.getchannel('A')) < 128)
    return res, {'shift': [dx, dy], 'diff': round(d, 1), 'diff_unshifted': round(d0, 1),
                 'magenta_fixed': int(bad.sum()), 'magenta_share': round(float(bad.sum()) / max(1, int(opaque.sum())), 4),
                 'size': list(size), 'transparent_share': round(float(flat.mean()), 3)}


def main():
    args = sys.argv[1:]
    root = DEFAULT_ROOT
    if '--root' in args:
        i = args.index('--root'); root = args[i + 1]; del args[i:i + 2]
    root = pathlib.Path(root)
    for n in args:
        rd = root / ('redraw%s' % n)
        boxes = json.loads((rd / 'boxes.json').read_text(encoding='utf-8'))
        (rd / 'compare').mkdir(exist_ok=True)
        sheets, report = {}, {}
        for key, b in sorted(boxes.items()):
            got = rd / 'received' / (b['sheet'] + '.png')
            if not got.exists():
                print('%s: %s not received' % (key, b['sheet'])); continue
            if b['sheet'] not in sheets:
                sheets[b['sheet']] = Image.open(got).convert('RGB').resize(tuple(b['canvas']), Image.LANCZOS)
            orig = Image.open(root / 'input' / (key + '.png')).convert('RGBA')
            res, info = fit_one(sheets[b['sheet']], b, orig)
            for name in [key] + b['same']:
                dst = root / 'output' / (name + '.png')
                dst.parent.mkdir(parents=True, exist_ok=True)
                res.save(dst)
            left = orig.resize(res.size, Image.NEAREST)
            cmp = Image.new('RGBA', (res.width * 2 + 8, res.height), (GREY, GREY, GREY, 255))
            cmp.alpha_composite(left, (0, 0)); cmp.alpha_composite(res, (res.width + 8, 0))
            cmp.convert('RGB').save(rd / 'compare' / (key.replace('/', '_') + '.png'))
            report[key] = info
            print('%-34s shift %+3d,%+3d  diff %5.1f (unshifted %5.1f)  magenta fixed %5d (%.1f%%)  -> %dx%d' % (
                key, info['shift'][0], info['shift'][1], info['diff'], info['diff_unshifted'], info['magenta_fixed'],
                info['magenta_share'] * 100, *info['size']))
        (rd / 'fit.json').write_text(json.dumps(report, indent=1), encoding='utf-8')
        if report:
            d = [v['diff'] for v in report.values()]
            print('redraw%s: %d fitted, diff mean %.1f max %.1f, shifts %s' % (
                n, len(report), sum(d) / len(d), max(d), sorted({tuple(v['shift']) for v in report.values()})[:12]))


if __name__ == '__main__':
    main()
