"""Build the Windows development host (x86-64) with zig cc.

    python build_host.py [--only-gen] [-O1]
"""
import concurrent.futures as cf, glob, os, shutil, subprocess, sys

ROOT = os.path.dirname(os.path.abspath(__file__))
GEN = os.path.join(ROOT, 'gen')
RT = os.path.join(ROOT, 'runtime')
WATCH = '--watch' in sys.argv          # debug build with memory write watchpoints
OBJ = os.path.join(ROOT, 'build', 'obj_watch' if WATCH else 'obj')
OUT = os.path.join(ROOT, 'build', 'nfs4host_watch.exe' if WATCH else 'nfs4host.exe')


def zig():
    z = shutil.which('zig')
    if z:
        return z
    base = os.path.join(os.environ['LOCALAPPDATA'], 'Microsoft', 'WinGet', 'Packages')
    for d, _, files in os.walk(base):
        if 'zig.exe' in files:
            return os.path.join(d, 'zig.exe')
    sys.exit('zig not found')


def compile_one(z, src, opt, incs):
    rel = os.path.relpath(src, ROOT).replace(os.sep, '_').replace('.c', '.o')
    obj = os.path.join(OBJ, rel)
    if os.path.exists(obj) and os.path.getmtime(obj) > os.path.getmtime(src) and 'gen' in src:
        return obj, None
    # -fixup-byte-word-insts=false works around an LLVM x86 backend bug (X86FixupBWInsts turns a
    # partial byte load into movzx while the upper bits are still live; see tests/llvm_bug.c)
    cmd = [z, 'cc', '-c', opt, '-w', '-fno-strict-aliasing', '-fwrapv', '-g0', '-target', 'x86_64-windows-gnu',
           '-mllvm', '-fixup-byte-word-insts=false'] + (['-DRT_WATCH'] if WATCH else []) + \
          ['-I' + i for i in incs] + [src, '-o', obj]
    r = subprocess.run(cmd, capture_output=True, text=True)
    return obj, (r.stderr if r.returncode else None)


def main():
    opt = next((a for a in sys.argv[1:] if a.startswith('-O')), '-O1')
    os.makedirs(OBJ, exist_ok=True)
    z = zig()
    mods = [d for d in sorted(glob.glob(os.path.join(GEN, '*'))) if os.path.isdir(d)]
    incs = [RT] + mods
    srcs = []
    for d in mods:
        srcs += sorted(glob.glob(os.path.join(d, '*.c')))
    if '--only-gen' not in sys.argv:
        srcs += sorted(glob.glob(os.path.join(RT, '*.c')))
        srcs += sorted(glob.glob(os.path.join(RT, 'win', '*.c')))
    objs, errors = [], 0
    with cf.ThreadPoolExecutor(max_workers=min(8, os.cpu_count() or 1)) as ex:
        for obj, err in ex.map(lambda s: compile_one(z, s, opt, incs), srcs):
            objs.append(obj)
            if err:
                errors += 1
                print('---', obj)
                print('\n'.join(err.splitlines()[:25]))
    if errors:
        sys.exit('%d files failed' % errors)
    if '--only-gen' in sys.argv:
        print('generated code compiled OK (%d files)' % len(objs))
        return
    cmd = [z, 'cc', '-target', 'x86_64-windows-gnu', '-o', OUT] + objs + ['-lgdi32', '-luser32', '-lwinmm', '-ld3d11']
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        print(r.stderr[:6000])
        sys.exit('link failed')
    print('built', OUT)


if __name__ == '__main__':
    main()
