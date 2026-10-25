from pathlib import Path
import os,subprocess,sys,time
root=Path(__file__).resolve().parents[1]; logs=root/'logs'; logs.mkdir(exist_ok=True)
mods=[('nfs4.exe','nfs4',None),('voodoo2a.dll','v2a','0x01000000'),('eacsnd.dll','eacsnd','0x01100000')]
for filename,prefix,base in mods:
    cmd=[sys.executable,'-B',str(root/'recomp/x86rec.py'),str(root/'binaries'/filename),str(root/'recomp/gen'/prefix),prefix]
    if base:cmd+=['--base',base]
    print('Recompiling',filename,flush=True)
    with (logs/(prefix+'-recompile.log')).open('w',encoding='utf-8') as f:
        result=subprocess.run(cmd,cwd=root,stdout=f,stderr=subprocess.STDOUT)
    print(prefix,'exit',result.returncode,flush=True)
    if result.returncode:sys.exit(result.returncode)
print('Recompilation finished',flush=True)
