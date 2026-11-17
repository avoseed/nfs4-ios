# HD front end (menus, title, loading, pause and result screens, menu fonts) by tile replacement.
#
# The front end draws each picture as 1:1 square tiles (256/128/64/32), one Glide texture each, and the menu fonts
# as glyphs cut from 256 px font textures; button labels are baked into their own small textures. A picture cannot
# be made bigger in the game's files, so (as NFS3 does) the game keeps its pictures and every tile texture is
# swapped for a sharper one by content when it reaches the GPU (recomp/runtime/hd_textures.c, the same key and
# hdtex/ pack as the track textures). The tiles of a picture are only known from the running game:
#
#   1. collect: run the test host through the menus with NFS4_MENU_MAP=<file> and NFS4_DUMP_TEXTURES=<dump>
#      NFS4_DUMP_MENUS=1 (one line per menu triangle every 15th frame: frame, texture key, size, 3 x (x y s t))
#        py -3 -B tools/menu_hd.py collect <dump> <map> [<map> ...] [--root D:/upscale/NFS4]
#      puts each frame's tiles back together into whole pictures: runs of 1:1 tiles that meet edge to edge without
#      overlapping are one picture; any other texture (a font, a stretched or partly used texture) is a picture of
#      its own. -> <root>/input/MENU/<name>.png and input/menu.json (name -> w, h, tiles [key, x, y, w, h]).
#      Skipped: a picture whose tiles are all in earlier pictures or in the track pack (<root>/pack/hdtex), and a
#      picture seen in fewer than 3 logged frames (movie frames, fades). 3D triangles are not looked at.
#      Looked over by eye: an entry marked "skip": true (a car skin, scenery, a cross-fade tile) stays known
#      but is not sent or packed.
#   1b. files: py -3 -B tools/menu_hd.py files [<game data folder>] [--root ..]
#      adds the full-screen pictures straight from FEART (loading screens of every track, title, results; see
#      SCREEN_FILES), cut both ways the front end cuts a screen.
#   1c. add: py -3 -B tools/menu_hd.py add <dump> <key> [<key> ...] [--root ..]
#      adds single dumped textures as pictures of their own (what collect leaves out because it is on screen for
#      too few frames, such as the spinning globe's frames): -> input/MENU/<key[:10]>_<w>x<h>.png
#   2. bundle: py -3 -B tools/menu_hd.py bundle <N> [--max-sheets K] [--missing] [--root ..]
#      --missing: only pictures with a piece not yet fitted (no output/MENU/<piece>.png)
#      -> <root>/redraw<N>, <N+1>, ... in the track bundles' layout (send/, todo.txt, boxes.json, 프롬프트.md).
#      A screen-sized picture is split into pieces of at most 320x240 (plus a 24 px margin of context), one a sheet;
#      small pictures share sheets, enlarged up to 4x (two to a row at least; a long bar across the sheet). tools/redraw_fit.py fits the answers as usual (3x output).
#   3. pack: py -3 -B tools/menu_hd.py pack <out dir> [--root ..]
#      stitches the fitted pieces (output/MENU/...) at 3x and cuts every tile out -> <out>/hdtex/<key>.rgba.
#      A tile without a fitted picture is left out (the game's own stays).
import sys, json, struct, pathlib, collections, hashlib
import numpy as np
from PIL import Image

DEFAULT_ROOT = 'D:/upscale/NFS4'
SCALE = 3                      # output: every menu tile at 3x (32 -> 96)
PIECE_W, PIECE_H, MARGIN = 320, 240, 24
CANVAS, GAP, GREY, KEY = 1024, 64, (128, 128, 128), (255, 0, 255)
SMALL_SCALE = 4


def load_map(path):
    frames = collections.defaultdict(list)
    for line in open(path, encoding='utf-8'):
        p = line.split()
        if len(p) != 16:
            continue
        v = [tuple(map(float, p[4 + i * 4:8 + i * 4])) for i in range(3)]
        frames[int(p[0])].append((p[1], int(p[2]), int(p[3]), v))
    return frames


def read_tex(dump, key):
    p = dump / (key + '.rgba')
    if not p.exists():
        return None
    b = p.read_bytes()
    w, h = struct.unpack_from('<II', b, 4)
    return Image.frombytes('RGBA', (w, h), b[12:12 + 4 * w * h])


def quads(tris):
    """triangles -> quads in draw order: (key, w, h, x0, y0, x1, y1, s0, t0, s1, t1); two triangles of one texture
    sharing a rectangle are one quad"""
    out = []
    for key, w, h, v in tris:
        xs, ys, ss, ts = ([a[i] for a in v] for i in range(4))
        # 2D only: a screen-aligned right triangle (3D cars and scenery in the showroom and race are not)
        if len({round(x) for x in xs}) != 2 or len({round(y) for y in ys}) != 2:
            continue
        q = (key, w, h, min(xs), min(ys), max(xs), max(ys), min(ss), min(ts), max(ss), max(ts))
        if out and out[-1][0] == key and abs(out[-1][3] - q[3]) < .5 and abs(out[-1][4] - q[4]) < .5 and \
                abs(out[-1][5] - q[5]) < .5 and abs(out[-1][6] - q[6]) < .5:
            continue
        out.append(q)
    return out


def is_tile(q):
    key, w, h, x0, y0, x1, y1, s0, t0, s1, t1 = q
    return w == h and w >= 32 and abs((x1 - x0) - (s1 - s0)) < .6 and abs((y1 - y0) - (t1 - t0)) < .6 and \
        abs(x0 - round(x0)) < .1 and abs(y0 - round(y0)) < .1


def group(qs):
    """runs of 1:1 tiles meeting edge to edge -> pictures [(key, x, y, size)] in screen pixels; others alone"""
    pics, cur = [], []
    def touches(t, g):
        # some pictures (the car photos) step their tiles 30 px, overlapping 2: that still counts as meeting
        _, x, y, s = t
        edge = False
        for _, gx, gy, gs in g:
            ox = min(x + s, gx + gs) - max(x, gx)
            oy = min(y + s, gy + gs) - max(y, gy)
            if ox > 2 and oy > 2:
                return 'overlap'
            if (ox >= 0 and oy > 2) or (oy >= 0 and ox > 2):
                edge = True
        return 'edge' if edge else None
    for q in qs:
        if not is_tile(q):
            if cur:
                pics.append(('tiles', cur)); cur = []
            pics.append(('alone', q))
            continue
        t = (q[0], round(q[3] - q[7]), round(q[4] - q[8]), q[1])
        if cur and touches(t, cur) == 'edge':
            cur.append(t)
        else:
            if cur:
                pics.append(('tiles', cur))
            cur = [t]
    if cur:
        pics.append(('tiles', cur))
    return pics


def collect(root, dump, maps):
    out = root / 'input' / 'MENU'
    out.mkdir(parents=True, exist_ok=True)
    path = root / 'input' / 'menu.json'
    index = json.loads(path.read_text(encoding='utf-8')) if path.exists() else {}
    known = {t[0] for e in index.values() for t in e['tiles']}
    # textures the track pack already replaces (the race HUD, redraw2) are not menu pictures
    known |= {p.stem for p in (root / 'pack' / 'hdtex').glob('*.rgba')}
    missing = skipped = moving = 0
    for m in maps:
        frames = load_map(m)
        found = []
        for f in sorted(frames):
            for kind, g in group(quads(frames[f])):
                if kind == 'alone':
                    key, w, h = g[0], g[1], g[2]
                    tiles = [(key, 0, 0, w, h)]
                    ox = oy = 0; W, H = w, h
                else:
                    ox = min(t[1] for t in g); oy = min(t[2] for t in g)
                    W = max(t[1] + t[3] for t in g) - ox; H = max(t[2] + t[3] for t in g) - oy
                    tiles = [(t[0], t[1] - ox, t[2] - oy, t[3], t[3]) for t in g]
                found.append((f, ox, oy, W, H, tiles))
        # a picture seen in under 3 logged frames (every 15th frame) is a movie frame or the middle of a fade
        seen = collections.Counter(frozenset(t[0] for t in tiles) for f, ox, oy, W, H, tiles in found)
        photos = {(ox, oy, W, H) for f, ox, oy, W, H, tiles in found if len(tiles) >= 4 and W >= 128 and H >= 128}
        inside = lambda x, y: any(px <= x and x + 32 <= px + pw and py <= y and y + 32 <= py + ph for px, py, pw, ph in photos)
        for f, ox, oy, W, H, tiles in found:
            if seen[frozenset(t[0] for t in tiles)] < 3:
                moving += 1; continue
            if all(t[0] in known for t in tiles):
                skipped += 1; continue
            img = Image.new('RGBA', (W, H), (0, 0, 0, 0))
            ok = True
            for key, x, y, w, h in tiles:
                t = read_tex(dump, key)
                if t is None:
                    ok = False; break
                img.paste(t, (x, y))
            if not ok:
                missing += 1; continue
            if len(tiles) == 1 and W == H == 32 and inside(ox, oy) and                     np.asarray(img.getchannel('A')).mean() > 230:
                moving += 1; continue       # a lone tile in a photo's place: its cross-fade, tile by tile
            name = '%s_%dx%d' % (hashlib.sha1(''.join(sorted(t[0] for t in tiles)).encode()).hexdigest()[:10], W, H)
            img.save(out / (name + '.png'))
            a = set(np.unique(np.asarray(img.getchannel('A'))).tolist())
            index['MENU/' + name] = {'w': W, 'h': H, 'screen': [ox, oy], 'frame': f, 'map': pathlib.Path(m).name,
                                     'alpha': min(a) < 255, 'soft': bool(a - {0, 255}), 'tiles': tiles}
            known.update(t[0] for t in tiles)
    path.write_text(json.dumps(index, indent=1), encoding='utf-8')
    sizes = collections.Counter((e['w'], e['h']) for e in index.values())
    print('%d pictures (%d tiles); %d already covered, %d moving, %d with a tile missing from the dump; sizes %s' % (
        len(index), len(known), skipped, moving, missing, sizes.most_common(8)))

# Full-screen pictures straight from the game's files: the front end cuts a 640x480 screen either into 32 px tiles
# (the loading screens, checked against a run) or into 256/128/64/32 tiles, largest first, column by column (the menu
# backgrounds); both cuts are listed, so either one finds its redraw. A screen the game draws something onto in
# memory first matches only where it is untouched (the run-time collect covers those).
SCREEN_FILES = ['SLIDES/T*_00.QFS', 'SLIDES/ENG.QFS', 'TITLE_E.QFS', 'MMNU.QFS', 'COMPARE.QFS', 'CONTROL.QFS',
                'CR_CIRC.QFS', 'TROPHY.QFS']


def tiling(cols=20, rows=15):
    occ = np.zeros((cols, rows), bool)
    occ[:, :] = True
    out = []
    for x in range(cols):
        for y in range(rows):
            s = 8
            while s:
                if x + s <= cols and y + s <= rows and occ[x:x + s, y:y + s].all():
                    occ[x:x + s, y:y + s] = False
                    out.append((x * 32, y * 32, s * 32))
                    break
                s //= 2
    return out


def files(root, game):
    sys.path.insert(0, str(pathlib.Path(__file__).parent))
    from redraw_export import refpack, fsh
    from hd_texture_pack import game_key
    out = root / 'input' / 'MENU'
    out.mkdir(parents=True, exist_ok=True)
    path = root / 'input' / 'menu.json'
    index = json.loads(path.read_text(encoding='utf-8')) if path.exists() else {}
    known = {t[0] for e in index.values() for t in e['tiles']}
    cuts = sorted({(x, y, 32) for x in range(0, 640, 32) for y in range(0, 480, 32)} | set(tiling()))
    added = 0
    for pattern in SCREEN_FILES:
        for f in sorted((game / 'FEART').glob(pattern)):
            for n, code, w, h, img in fsh(refpack(f.read_bytes())):
                if img is None or (w, h) != (640, 480):
                    continue
                img = img.convert('RGBA')
                tiles = [(game_key(img.crop((x, y, x + s, y + s))), x, y, s, s) for x, y, s in cuts]
                grid = [t for t in tiles if t[3] == 32]
                if sum(t[0] in known for t in grid) >= 0.9 * len(grid):
                    continue
                name = 'F_%s_%s' % (f.stem, ''.join(c if c.isalnum() else '_' for c in n))
                img.save(out / (name + '.png'))
                index['MENU/' + name] = {'w': w, 'h': h, 'screen': [0, 0], 'frame': 0, 'map': 'file ' + f.name,
                                         'alpha': False, 'soft': False, 'tiles': tiles}
                known.update(t[0] for t in tiles)
                added += 1
                print('%s/%s' % (f.name, n))
    path.write_text(json.dumps(index, indent=1), encoding='utf-8')
    print('%d screens added from the game files' % added)


def pieces(e):
    """a picture -> [(piece name suffix, crop box with margin, inner box)]"""
    W, H = e['w'], e['h']
    if W * H <= 400 * 400:              # a car photo or a long bar whole; a screen in pieces
        return [('', (0, 0, W, H), (0, 0, W, H))]
    nx, ny = -(-W // PIECE_W), -(-H // PIECE_H)
    out = []
    for j in range(ny):
        for i in range(nx):
            ix0, ix1 = W * i // nx, W * (i + 1) // nx
            iy0, iy1 = H * j // ny, H * (j + 1) // ny
            crop = (max(0, ix0 - MARGIN), max(0, iy0 - MARGIN), min(W, ix1 + MARGIN), min(H, iy1 + MARGIN))
            out.append(('__p%d%d' % (j, i), crop, (ix0, iy0, ix1, iy1)))
    return out


MENU_WHAT = '''- 모든 입력은 레이싱 게임의 메뉴 화면 그림(배경, 자동차 사진, 버튼, 제목 글자, 글꼴 판, 아이콘)을 회색 바탕 위에 확대해 놓은 "모음 그림"이다. 큰 그림은 한 장에 하나(화면 그림의 한 조각)이고, 작은 그림은 여러 장을 간격을 두고 늘어놓았다.'''

MENU_INSTRUCTIONS = '''첨부한 그림은 1999년 PC 레이싱 게임(니드 포 스피드 4)의 메뉴 화면 그림을 확대해서 회색 바탕에 놓은 것이다. 화면 배경이나 자동차 사진의 한 조각일 수도 있고, 버튼·제목 글자·아이콘·글꼴 판 같은 작은 그림 여러 장일 수도 있다. 원본은 640x480 화면용의 작은 그림이라 흐릿하고 계단 모양이 보인다. 원작 그대로 다시 그려서, 같은 그림의 선명한 고해상도판을 만들어 줘.
1. 칸(회색 바탕 위의 네모 그림)마다 따로따로, 제자리에서, 그 칸 크기 그대로 다시 그린다. 칸끼리 이어 그리거나, 섞거나, 옮기거나, 크기를 바꾸지 않는다. 칸 사이와 둘레의 회색은 단색 회색 그대로 둔다.
2. 구도를 그대로 둔다. 선, 테두리, 둥근 버튼 모양, 글자, 자동차와 배경의 위치와 크기가 원본과 정확히 겹쳐야 한다. 확대·축소·이동·회전·뒤집기를 하지 않는다. 그림이 칸 가장자리에서 잘려 있으면 잘린 그대로 둔다(화면 그림의 한 조각이라 옆 조각과 이어 붙인다).
3. 내용을 그대로 둔다. 새 물건·장식·무늬·빛을 더하지 않고, 있는 것을 빼지 않는다.
4. 색과 빛을 그대로 둔다. 색감, 밝기, 채도, 그러데이션, 광택을 원본과 같게 한다. 더 밝게, 더 진하게, 더 화사하게 만들지 않는다.
5. 화풍은 원본 그대로다. 버튼·테두리·아이콘·글자는 깔끔한 게임 그래픽(평평한 색과 단순한 그러데이션, 매끈한 곡선과 직선)으로, 자동차와 배경 사진은 사실적인 사진으로 또렷하게 한다. 만화풍·수채화풍으로 바꾸지 않는다.
6. 글자와 숫자는 같은 글자, 같은 글꼴 모양, 같은 위치와 크기, 같은 색으로, 윤곽이 매끈한 벡터 글자처럼 또렷하게만 한다. 도트나 계단 모양이 남으면 안 된다. 글자를 바꾸거나 지어내거나 빼지 않는다. 글꼴 판(글자가 한 장에 빽빽하게 모인 그림)은 글자마다 자리와 크기를 그대로 두고 글자 모양만 또렷하게 한다.
7. 순수한 자홍색(마젠타, #FF00FF) 부분은 게임에서 투명하게 뚫리는 자리다. 자홍색 자리는 자홍색 그대로 둔다. 그림과 자홍색의 경계에서는 그림의 색을 경계까지 꽉 채우고, 그림 색과 자홍색을 섞은 분홍·보라색 테두리는 만들지 않는다. 경계의 정확한 모양은 내가 원본으로 다시 맞추므로 신경 쓰지 않아도 된다.
8. 칸 가장자리에 새 테두리, 액자, 그림자, 어두워짐(비네팅)을 넣지 않는다.
9. 결과는 첨부한 그림과 같은 비율, 같은 크기로 만든다. 서명, 워터마크, 원본에 없는 글자를 넣지 않는다.

'''


def place_small(items):
    """shelf-pack [(name, img)] (already enlarged) onto 1024 sheets with GAP between -> [[(name, x, y, img)]]"""
    sheets, cur, x, y, row = [], [], GAP, GAP, 0
    for name, img in sorted(items, key=lambda t: -t[1].height):
        w, h = img.size
        if x + w > CANVAS - GAP:
            x, y, row = GAP, y + row + GAP, 0
        if y + h > CANVAS - GAP:
            sheets.append(cur); cur, x, y, row = [], GAP, GAP, 0
        cur.append((name, x, y, img)); x += w + GAP; row = max(row, h)
    if cur:
        sheets.append(cur)
    return sheets


def sheet_image(img):
    """RGBA -> RGB on grey, see-through as flat magenta"""
    a = np.asarray(img)
    rgb = a[..., :3].copy()
    rgb[a[..., 3] < 128] = KEY
    return Image.fromarray(rgb, 'RGB')


def add(root, dump, keys):
    path = root / 'input' / 'menu.json'
    index = json.loads(path.read_text(encoding='utf-8'))
    known = {t[0] for e in index.values() for t in e['tiles']}
    for key in keys:
        if key in known:
            print('%s: already known' % key)
            continue
        img = read_tex(dump, key)
        name = 'MENU/%s_%dx%d' % (key[:10], img.width, img.height)
        img.save(root / 'input' / (name + '.png'))
        a = np.asarray(img.getchannel('A'))
        index[name] = {'w': img.width, 'h': img.height, 'screen': [0, 0], 'frame': 0, 'map': 'dump ' + dump.name,
                       'alpha': bool((a < 255).any()), 'soft': bool(((a > 0) & (a < 255)).any()),
                       'tiles': [[key, 0, 0, img.width, img.height]]}
        known.add(key)
        print(name)
    path.write_text(json.dumps(index, indent=1), encoding='utf-8')


def bundle(root, first, max_sheets, missing=False):
    index = json.loads((root / 'input' / 'menu.json').read_text(encoding='utf-8'))
    big, small = [], []
    for name, e in sorted(index.items(), key=lambda t: (t[1]['map'], t[1]['frame'])):
        if e.get('skip'):
            continue
        if missing and all((root / 'output' / (name + suffix + '.png')).exists() for suffix, _, _ in pieces(e)):
            continue
        img = Image.open(root / 'input' / (name + '.png')).convert('RGBA')
        for suffix, crop, inner in pieces(e):
            part = img.crop(crop)
            pname = name + suffix
            if suffix:
                part.save(root / 'input' / (pname + '.png'))
            seethrough = float((np.asarray(part.getchannel('A')) < 128).mean())
            entry = {'w': part.width, 'h': part.height, 'alpha': e['alpha'], 'soft': e['soft'], 'same': [],
                     'search': 0 if seethrough > 0.5 else 6,
                     'out_max': SCALE * max(part.size), 'picture': name, 'crop': list(crop), 'inner': list(inner)}
            if suffix or part.width * part.height > 200 * 200:
                s = min((CANVAS - 2 * GAP) / part.width, (CANVAS - 2 * GAP) / part.height)
                big.append((pname, entry, part, s))
            else:
                small.append((pname, entry, part))
    sheets = []
    for pname, entry, part, s in big:
        im = sheet_image(part).resize((round(part.width * s), round(part.height * s)), Image.BICUBIC)
        x, y = (CANVAS - im.width) // 2, (CANVAS - im.height) // 2
        sheets.append([(pname, entry, x, y, im)])
    smalls = {pname: (entry, part) for pname, entry, part in small}
    def enlarge(part):
        k = min(SMALL_SCALE, (CANVAS - 3 * GAP) / 2 / max(part.size)) if max(part.size) <= 224 else (CANVAS - 2 * GAP) / max(part.size)
        return sheet_image(part).resize((round(part.width * k), round(part.height * k)), Image.BICUBIC)
    for sh in place_small([(p, enlarge(part)) for p, e, part in small]):
        sheets.append([(p, smalls[p][0], x, y, im) for p, x, y, im in sh])
    n, made = first, []
    for start in range(0, len(sheets), max_sheets):
        rd = root / ('redraw%d' % n)
        if rd.exists():
            raise SystemExit('%s exists' % rd)
        (rd / 'send').mkdir(parents=True); (rd / 'received').mkdir()
        boxes, todo = {}, []
        for i, sh in enumerate(sheets[start:start + max_sheets]):
            sname = 't_sheet_MENU_s%02d' % i
            canvas = Image.new('RGB', (CANVAS, CANVAS), GREY)
            for pname, entry, x, y, im in sh:
                canvas.paste(im, (x, y))
                boxes[pname] = dict(entry, sheet=sname, canvas=[CANVAS, CANVAS], box=[x, y, im.width, im.height])
            canvas.save(rd / 'send' / (sname + '.png'))
            todo.append(sname)
        (rd / 'boxes.json').write_text(json.dumps(boxes, indent=1), encoding='utf-8')
        (rd / 'todo.txt').write_text('\n'.join(todo) + '\n', encoding='utf-8')
        sys.path.insert(0, str(pathlib.Path(__file__).parent))
        import redraw_batches
        (rd / '프롬프트.md').write_text(redraw_batches.PROMPT.format(n=n, folder=str(rd), count=len(todo), what=MENU_WHAT,
                                        instructions=MENU_INSTRUCTIONS), encoding='utf-8')
        made.append((rd, len(todo), len(boxes)))
        n += 1
    for rd, s, b in made:
        print('%s: %d sheets, %d pictures/pieces' % (rd, s, b))


def pack(root, out):
    index = json.loads((root / 'input' / 'menu.json').read_text(encoding='utf-8'))
    dst = out / 'hdtex'
    dst.mkdir(parents=True, exist_ok=True)
    written, done_pics = set(), 0
    for name, e in index.items():
        if e.get('skip'):
            continue
        W, H = e['w'] * SCALE, e['h'] * SCALE
        acc = np.zeros((H, W, 4), np.float32); wt = np.zeros((H, W, 1), np.float32)
        have = True
        for suffix, crop, inner in pieces(e):
            p = root / 'output' / (name + suffix + '.png')
            if not p.exists():
                have = False; break
            im = np.asarray(Image.open(p).convert('RGBA').resize(((crop[2] - crop[0]) * SCALE, (crop[3] - crop[1]) * SCALE), Image.LANCZOS), np.float32)
            # weight: 1 inside the inner box, fading to 0 over the margin
            hh, ww = im.shape[:2]
            yy, xx = np.mgrid[0:hh, 0:ww].astype(np.float32)
            gx = xx / SCALE + crop[0]; gy = yy / SCALE + crop[1]
            def ramp(g, a, b, lo, hi):
                r = np.ones_like(g)
                if a > lo: r = np.minimum(r, np.clip((g - (a - MARGIN)) / (2 * MARGIN), 0, 1))
                if b < hi: r = np.minimum(r, np.clip(((b + MARGIN) - g) / (2 * MARGIN), 0, 1))
                return r
            w8 = (ramp(gx, inner[0], inner[2], 0, e['w']) * ramp(gy, inner[1], inner[3], 0, e['h']))[..., None] + 1e-6
            y0, x0 = crop[1] * SCALE, crop[0] * SCALE
            acc[y0:y0 + hh, x0:x0 + ww] += im * w8; wt[y0:y0 + hh, x0:x0 + ww] += w8
        if not have:
            continue
        hd = np.clip(acc / np.maximum(wt, 1e-6) + .5, 0, 255).astype(np.uint8)
        done_pics += 1
        for key, x, y, w, h in e['tiles']:
            if key in written:
                continue
            t = Image.fromarray(hd[y * SCALE:(y + h) * SCALE, x * SCALE:(x + w) * SCALE], 'RGBA')
            (dst / (key + '.rgba')).write_bytes(struct.pack('<4sII', b'HDT1', t.width, t.height) + t.tobytes())
            written.add(key)
    print('%s: %d pictures, %d tiles' % (dst, done_pics, len(written)))


def main():
    args = sys.argv[1:]
    root = DEFAULT_ROOT
    if '--root' in args:
        i = args.index('--root'); root = args[i + 1]; del args[i:i + 2]
    root = pathlib.Path(root)
    max_sheets = 40
    if '--max-sheets' in args:
        i = args.index('--max-sheets'); max_sheets = int(args[i + 1]); del args[i:i + 2]
    cmd = args[0]
    if cmd == 'collect':
        collect(root, pathlib.Path(args[1]), args[2:])
    elif cmd == 'files':
        files(root, pathlib.Path(args[1]) if len(args) > 1 else pathlib.Path(__file__).resolve().parents[1] / 'game' / 'data')
    elif cmd == 'add':
        add(root, pathlib.Path(args[1]), args[2:])
    elif cmd == 'bundle':
        missing = '--missing' in args
        if missing:
            args.remove('--missing')
        bundle(root, int(args[1]), max_sheets, missing)
    elif cmd == 'pack':
        pack(root, pathlib.Path(args[1]))
    else:
        raise SystemExit(__doc__)


if __name__ == '__main__':
    main()
