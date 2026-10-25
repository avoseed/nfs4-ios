from pathlib import Path
import os,subprocess,sys
root=Path(__file__).resolve().parents[1]; env=os.environ.copy()
for name,sub in [('TMP','tmp'),('TEMP','tmp'),('ZIG_GLOBAL_CACHE_DIR','.cache/zig-global'),('ZIG_LOCAL_CACHE_DIR','.cache/zig-local')]:
 p=root/sub;p.mkdir(parents=True,exist_ok=True);env[name]=str(p)
with (root/'logs/build-host.log').open('w',encoding='utf-8') as f:
 r=subprocess.run([sys.executable,'-B',str(root/'recomp/build_host.py'),'-O1'],cwd=root,env=env,stdout=f,stderr=subprocess.STDOUT)
print('Build exit:',r.returncode)
print((root/'logs/build-host.log').read_text(encoding='utf-8')[-5000:])
sys.exit(r.returncode)
