# Mirror twins made consistent after the fit (tools/redraw_fit.py).
#   py -3 tools/redraw_mirror.py [--root D:/upscale/NFS4] TRACK ...
# A track often holds the same picture flipped (signs seen from both sides, the night file's copies): TR0_018 is
# TR0_031 upside down, texel for texel. The image model draws each on its own and the answers drift apart (one
# keeps the mirror writing, the other does not). Pictures of output/<TRACK> whose originals (input/) are exact
# flips or turns of each other form a group; the answer whose shrunken copy is nearest its own original - and
# nearer to it than to the flipped original, so a wrongly turned answer is never chosen - is kept, and the
# others are rewritten as its flip. Members left out of output/ (held back for a resend) are filled in too.
import sys, pathlib
import numpy as np
from PIL import Image

DEFAULT_ROOT = 'D:/upscale/NFS4'
FLIPS = {'id': lambda x: x, 'lr': lambda x: x[:, ::-1], 'ud': lambda x: x[::-1], 'rot': lambda x: x[::-1, ::-1]}


def dist(ans, o):
    """How far the answer, shrunk to the original's size, is from the picture o (opaque texels)."""
    small = np.asarray(ans.convert('RGBA').resize((o.shape[1], o.shape[0]), Image.BOX)).astype(float)
    o = o.astype(float)
    m = (o[..., 3] > 127) & (small[..., 3] > 127)
    return float(np.abs(small[..., :3] - o[..., :3])[m].mean()) if m.any() else 0.0


def main():
    args = sys.argv[1:]
    root = pathlib.Path(DEFAULT_ROOT)
    if '--root' in args:
        i = args.index('--root'); root = pathlib.Path(args[i + 1]); del args[i:i + 2]
    for track in args:
        out = root / 'output' / track
        done = {p.stem for p in out.glob('*.png')}
        orig = {p.stem: np.asarray(Image.open(p).convert('RGBA')) for p in (root / 'input' / track).glob('*.png')}
        # group: key -> (group leader, flip from leader to key)
        group = {}
        keys = sorted(orig)
        for i, a in enumerate(keys):
            if a in group or not orig[a].std():
                continue
            members = {a: 'id'}
            for b in keys[i + 1:]:
                if b in group or orig[a].shape != orig[b].shape:
                    continue
                for f in ('lr', 'ud', 'rot'):
                    if (FLIPS[f](orig[a]) == orig[b]).all() and not (orig[a] == orig[b]).all():
                        members[b] = f; break
            if len(members) > 1 and done & set(members):
                for k, f in members.items():
                    group[k] = (a, f)
        groups = {}
        for k, (lead, f) in group.items():
            groups.setdefault(lead, {})[k] = f
        changed = 0
        for lead, members in sorted(groups.items()):
            best = None
            for k in members:
                if k not in done:
                    continue
                ans = Image.open(out / (k + '.png'))
                own = dist(ans, orig[k])
                turned = [m for m in members if members[m] != members[k]]   # the same picture in another turn
                other = min(dist(ans, orig[m]) for m in turned)
                if own < other and (best is None or own < best[1]):
                    best = (k, own, ans)
            if best is None:
                print('%s %s: no answer keeps its own turn, left as is' % (track, lead)); continue
            src, own, ans = best
            a = np.asarray(ans.convert('RGBA'))
            a = FLIPS[members[src]](a)                                  # back to the leader's turn
            for k, f in members.items():
                if k == src:
                    continue
                Image.fromarray(np.ascontiguousarray(FLIPS[f](a))).save(out / (k + '.png'))
                changed += 1
            print('%s %s: from %s (diff %.1f) -> %s' % (track, lead, src, own, ', '.join(k for k in members if k != src)))
        print('%s: %d groups, %d pictures rewritten' % (track, len(groups), changed))


if __name__ == '__main__':
    main()
