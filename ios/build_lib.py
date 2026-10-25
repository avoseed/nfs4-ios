"""Recompile the game binaries and build libnfs4.a for iOS (run on macOS).

    python3 ios/build_lib.py [--jobs N]

Inputs:  binaries/nfs4.exe, binaries/voodoo2a.dll, binaries/eacsnd.dll
Output:  ios/build/libnfs4.a  (recompiled game + runtime, arm64)
"""
import concurrent.futures as cf, glob, os, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RECOMP = os.path.join(ROOT, 'recomp')
GEN = os.path.join(RECOMP, 'gen')
OBJ = os.path.join(ROOT, 'ios', 'build', 'obj')
LIB = os.path.join(ROOT, 'ios', 'build', 'libnfs4.a')
BIN = os.path.join(ROOT, 'binaries')

MODULES = [  # file, prefix, base
    ('nfs4.exe', 'nfs4', None),
    ('voodoo2a.dll', 'v2a', '0x01000000'),
    ('eacsnd.dll', 'eacsnd', '0x01100000'),
]


def sh(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.stderr.write(r.stdout + r.stderr)
        raise SystemExit('failed: ' + ' '.join(cmd[:6]))
    return r.stdout


def main():
    jobs = int(sys.argv[sys.argv.index('--jobs') + 1]) if '--jobs' in sys.argv else min(4, os.cpu_count() or 1)
    os.makedirs(OBJ, exist_ok=True)
    for f, prefix, base in MODULES:
        cmd = [sys.executable, os.path.join(RECOMP, 'x86rec.py'), os.path.join(BIN, f), os.path.join(GEN, prefix), prefix]
        if base:
            cmd += ['--base', base]
        print(sh(cmd).strip().splitlines()[-1], flush=True)

    sdk = sh(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path']).strip()
    clang = sh(['xcrun', '--sdk', 'iphoneos', '-f', 'clang']).strip()
    incs = [os.path.join(RECOMP, 'runtime')] + [os.path.join(GEN, p) for _, p, _ in MODULES]
    srcs = sorted(glob.glob(os.path.join(GEN, '*', '*.c')))
    srcs += [s for s in sorted(glob.glob(os.path.join(RECOMP, 'runtime', '*.c')))]
    flags = ['-arch', 'arm64', '-isysroot', sdk, '-miphoneos-version-min=15.0', '-O2', '-w',
             '-fno-strict-aliasing', '-fwrapv', '-g0'] + ['-I' + i for i in incs]

    def build(src):
        obj = os.path.join(OBJ, os.path.relpath(src, ROOT).replace(os.sep, '_')[:-2] + '.o')
        r = subprocess.run([clang, '-c'] + flags + [src, '-o', obj], capture_output=True, text=True)
        return obj, src, (r.stderr if r.returncode else None)

    objs, failed = [], 0
    with cf.ThreadPoolExecutor(max_workers=jobs) as ex:
        for obj, src, err in ex.map(build, srcs):
            objs.append(obj)
            if err:
                failed += 1
                print('--- ' + src + '\n' + '\n'.join(err.splitlines()[:30]))
            else:
                print('compiled', os.path.basename(src), flush=True)
    if failed:
        raise SystemExit('%d files failed' % failed)
    if os.path.exists(LIB):
        os.remove(LIB)
    sh(['xcrun', 'ar', 'rcs', LIB] + objs)
    print('built', LIB)


if __name__ == '__main__':
    main()
