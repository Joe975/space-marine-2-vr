"""Two 30s qualified captures inside one fixed 100s automated pose. Args: ABS_OUTPUT PID PREFIX present isolated (or reversed). Resets control to present."""
from pathlib import Path
import csv, datetime, json, subprocess, sys, time, uuid
root=Path(__file__).resolve().parents[1];sys.path.insert(0,str(root/'scripts'))
from vr_test_driver import compile_script,write_request,mailbox,report_trace
from benchmark_driver_performance import read_status
from qualify_driver_performance import qualify,qualify_allocation,load_records
from analyze_performance import analyze
from analyze_render_probes import analyze as probes
if len(sys.argv)!=6:raise SystemExit(__doc__)
out=Path(sys.argv[1]);pid=sys.argv[2];prefix=sys.argv[3];modes=sys.argv[4:]
assert out.is_absolute() and out.is_dir() and len(modes)==2 and set(modes)=={'present','isolated'}
assert not list(out.glob(prefix+'*'))
directory=mailbox();rid=uuid.uuid4().hex
scenario=json.loads((root/'config/test_driver/performance_stationary.json').read_text())
scenario['duration_ms']=100000;scenario['frames'][-1]['ms']=100000
def save(suffix,value):(out/(prefix+suffix)).write_text(json.dumps(value,indent=2)+'\n')
save('.request.json',{'id':rid,'scenario':scenario,'mailbox':str(directory)})
clock={'utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'qpc_ms':time.perf_counter_ns()/1e6};save('.clock.json',clock);statuses=[]
def status():
 s=read_status(directory/'status.json');statuses.append(dict(s,qpc_ms=time.perf_counter_ns()/1e6));return s
def check():
 s=status()
 if s['id']!=rid or s['state']!='running' or s['error']:raise RuntimeError(s)
def mode(value):
 pending=out/'handoff.pending';pending.write_text(value+'\n',encoding='ascii');pending.replace(out/'handoff.txt')
assert status()['state'] not in ('running','preroll')
mode(modes[0]);write_request(directory,compile_script(scenario,rid));deadline=time.monotonic()+20
process=None
try:
 while True:
  s=status()
  if s['id']==rid and s['state']=='running' and s['elapsed_ms']>=2000:break
  if time.monotonic()>deadline:raise RuntimeError(s)
  time.sleep(.2)
 for strategy in modes:
  mode(strategy);label=prefix+'_'+strategy
  with (out/(label+'.capture.log')).open('w') as log:
   process=subprocess.Popen(['pwsh','-NoProfile','-File',str(root/'scripts/capture-performance.ps1'),'-GameProcessId',pid,'-OutputDirectory',str(out),'-Label',label,'-Mode','full','-Seconds','30','-WarmupSeconds','5'],stdout=log,stderr=subprocess.STDOUT)
   while process.poll() is None:check();time.sleep(.25)
   if process.returncode:raise RuntimeError(process.returncode)
 deadline=time.monotonic()+40
 while True:
  s=status()
  if s['id']!=rid or s['error']:raise RuntimeError(s)
  if s['state']=='completed':break
  if s['state']!='running' or time.monotonic()>deadline:raise RuntimeError(s)
  time.sleep(.25)
 trace=out/(prefix+'.trace.jsonl');trace.write_bytes((Path.home()/'AppData/Local/sm2vr/sm2vr_trace.jsonl').read_bytes());records=load_records(trace)
 for strategy in modes:
  label=prefix+'_'+strategy
  with (out/(label+'.csv')).open(encoding='utf-8-sig',newline='') as pm,(out/'cpu_trace.csv').open(encoding='utf-8-sig',newline='') as cpu:summary=analyze(pm,cpu,'full',True,'single_frame')
  result=qualify(summary,clock,statuses,records,rid)
  with (out/'cpu_trace.csv').open(encoding='utf-8-sig',newline='') as cpu:result['allocation']=qualify_allocation(csv.DictReader(cpu),summary,'pooled')
  with (out/'cpu_trace.csv').open(encoding='utf-8-sig',newline='') as cpu:probe=probes(csv.DictReader(cpu),summary['qpc_start_ms'],summary['qpc_end_ms'])
  assert probe['handoff_modes']==[(3 if strategy=='isolated' else 0)],probe['handoff_modes']
  assert probe['isolated_bridge_values']==[int(strategy=='isolated')],probe['isolated_bridge_values']
  assert not probe['refused_pairs'] and not probe['gpu_probe_errors'],probe
  assert probe['joined_pairs']>=.99*probe['accepted_pairs'],probe['incomplete_accepted_pairs']
  for suffix,value in (('.qualification.json',result),('.summary.json',summary),('.probes.json',probe)):(out/(label+suffix)).write_text(json.dumps(value,indent=2)+'\n')
  print(json.dumps({'label':label,'fps':summary['fresh_pairs_per_second'],'p99':summary['present_interval_ms']['p99'],'qualification':result,'intervals':probe['intervals_ms'],'execution':probe['pre_upscale_execution'],'submit_points':probe['submit_points']}),flush=True)
 save('.driver.json',report_trace(trace.read_text().splitlines(),rid))
finally:
 save('.statuses.json',statuses)
 if process is not None and process.poll() is None:process.wait(timeout=45)
 mode('present')
