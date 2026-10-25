from pathlib import Path
import os,sys,subprocess,json,time
r=Path(__file__).resolve().parents[1];label=sys.argv[1] if len(sys.argv)>1 else 'boot';secs=float(sys.argv[2]) if len(sys.argv)>2 else 20
save=r/'recomp/run'/label;save.mkdir(parents=True,exist_ok=True)
env=os.environ.copy();env.update({'TEMP':str(r/'tmp'),'TMP':str(r/'tmp'),'NFS4_SHOTS':'3','NFS4_DUMP_AFTER':'10','NFS4_TRACE_FILES':'1'})
if len(sys.argv)>3 and sys.argv[3]:env['NFS4_KEYS']=sys.argv[3]
if len(sys.argv)>4 and sys.argv[4]:env['NFS4_TRACE_AFTER']=sys.argv[4]
if len(sys.argv)>5 and sys.argv[5]:env['NFS4_WIDE']=sys.argv[5]
if len(sys.argv)>6 and sys.argv[6]:env['NFS4_WAV']=sys.argv[6]
cmd=[str(r/'recomp/build/nfs4host.exe'),str(r/'game'),str(save)]
started=time.monotonic(); stopped_by_test=False
with (save/'stdout.log').open('w',encoding='utf-8') as f:
 p=subprocess.Popen(cmd,cwd=r,env=env,stdout=f,stderr=subprocess.STDOUT,creationflags=subprocess.CREATE_NO_WINDOW)
 try:p.wait(timeout=secs)
 except subprocess.TimeoutExpired:
  stopped_by_test=True;p.terminate();p.wait();print('Stopped this test process after',secs,'seconds')
 print('Host exit:',p.returncode)
log=save/'nfs4host.log'
contents=log.read_text(encoding='utf-8',errors='replace') if log.exists() else ''
if contents:print(contents[-2500:])
trapped='TRAP at ' in contents or 'FATAL:' in contents
(save/'result.json').write_text(json.dumps({'label':label,'elapsed_seconds':round(time.monotonic()-started,2),'host_returncode':p.returncode,'stopped_by_test':stopped_by_test,'guest_trap':trapped,'script':sys.argv[3] if len(sys.argv)>3 else ''},indent=2),encoding='utf-8')
if trapped or (p.returncode!=0 and not stopped_by_test):raise SystemExit(1)

