"""Compile one generated test function in isolation and dump its assembly."""
import re, subprocess, sys, os
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import build_host
name = sys.argv[1]
src = open(os.path.join(HERE, 'build', 'dt_gen.c')).read()
m = re.search(r'void %s\(Cpu \*c\) \{.*?\n\}\n' % name, src, re.S)
body = '#define RT_FLAGS_FROM_CPU\n#include "rt.h"\n' + m.group(0)
path = os.path.join(HERE, 'build', 'one.c')
open(path, 'w').write(body)
rt = os.path.join(os.path.dirname(HERE), 'runtime')
subprocess.run([build_host.zig(), 'cc', '-S', '-O1', '-w', '-fno-strict-aliasing', '-fwrapv', '-masm=intel',
                '-target', 'x86_64-windows-gnu', '-I', rt, path, '-o', path[:-2] + '.s'], check=True)
for line in open(path[:-2] + '.s'):
    if not line.strip().startswith('.'):
        print(line.rstrip())
