from pathlib import Path
import os, subprocess, shutil
root=Path(__file__).resolve().parents[1]
env=os.environ.copy()
for name,sub in [('TEMP','tmp'),('TMP','tmp'),('ZIG_GLOBAL_CACHE_DIR','.cache/zig-global'),('ZIG_LOCAL_CACHE_DIR','.cache/zig-local')]:
    path=root/sub;path.mkdir(parents=True,exist_ok=True);env[name]=str(path)
exe=root/'recomp/tests/build/idle_policy.exe';exe.parent.mkdir(parents=True,exist_ok=True)
subprocess.run([shutil.which('zig'),'cc','-target','x86_64-windows-gnu','-O2','-I'+str(root/'recomp/runtime'),str(root/'recomp/tests/idle_policy.c'),'-o',str(exe)],env=env,check=True)
subprocess.run([str(exe),str(root/'tmp')],env=env,check=True)
