"""Load a build for a bounded device check; does not install a ZIP or restart the game."""
import argparse
import json
import pathlib
import subprocess
import time
import frida

ROOT = pathlib.Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser()
p.add_argument('--pid', type=int, required=True)
p.add_argument('--device-library', required=True)
p.add_argument('--library', default=str(ROOT/'build/readonly_native/libgcloudsync.so'))
p.add_argument('--output', required=True)
p.add_argument('--input-seconds', type=float, default=0)
p.add_argument('--companion-port', type=int, default=0)
a = p.parse_args()
if not 0 <= a.input_seconds <= 10: p.error('input-seconds must be 0..10')
nm = ROOT/'tools/downloads/android-ndk-r27c/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-nm.exe'
symbols = {}
for line in subprocess.check_output([str(nm), '--defined-only', a.library], text=True).splitlines():
    fields = line.split()
    if len(fields) == 3: symbols[fields[2]] = int(fields[0], 16)
source = r'''
let handle, module, scene, read, close, start, stop, status, configure;
const snapshots = Memory.alloc(6264);
function native(name, result, args, rvas) { return new NativeFunction(module.base.add(rvas[name]), result, args); }
function collect() {
  if (!read(scene, snapshots)) return {ready:false};
  const count=snapshots.add(6208).readU64().toNumber();
  const matrix=Array.from({length:16},(_,i)=>snapshots.add(i*4).readFloat());
  let candidates=[];
  for(let i=0;i<count;i++){
    const q=snapshots.add(64+i*24), p=[q.add(8).readFloat(),q.add(12).readFloat(),q.add(16).readFloat(),1];
    if(!q.add(20).readU8())continue;
    const clip=[0,1,2,3].map(r=>p.reduce((sum,x,c)=>sum+matrix[r*4+c]*x,0));
    if(clip[3]<=.01)continue;
    const error=[clip[0]/clip[3]*1472,-clip[1]/clip[3]*920], distance=Math.hypot(...error);
    candidates.push({id:q.readPointer().toString(),point:p.slice(0,3),error,distance,inside200:distance<=200});
  }
  candidates.sort((a,b)=>a.distance-b.distance);
  return {ready:true,frame:snapshots.add(6224).readS32(),projectionY:snapshots.add(6228).readFloat(),
          local:snapshots.add(6248).readPointer().toString(),builtinAim:!!snapshots.add(6256).readU8(),candidates};
}
rpc.exports={
  init(path,rvas){
    const dlopen=new NativeFunction(Process.getModuleByName('libdl.so').getExportByName('dlopen'),'pointer',['pointer','int']);
    handle=dlopen(Memory.allocUtf8String(path),2);
    if(handle.isNull())throw new Error('dlopen failed');
    module=Process.getModuleByName(path.split('/').pop());
    const find=native('cf_find_module','bool',['pointer','pointer'],rvas), info=Memory.alloc(160), error=Memory.alloc(256);
    if(!find(Memory.allocUtf8String('libil2cpp.so'),info))throw new Error('IL2CPP not loaded');
    scene=native('cf_scene_open','pointer',['pointer','pointer','ulong'],rvas)(info,error,256);
    if(scene.isNull())throw new Error('scene bind: '+error.readUtf8String());
    read=native('cf_scene_read','bool',['pointer','pointer'],rvas);
    close=native('cf_scene_close','void',['pointer'],rvas);
    start=native('cf_payload_start_vm','int',['pointer'],rvas);
    stop=native('cf_payload_stop','void',[],rvas);
    status=native('cf_payload_status','int',[],rvas);
    configure=native('cf_payload_input_fd','int',['int'],rvas);
    return {base:module.base.toString(),pid:Process.id,preview:collect()};
  },
  snapshot:collect,
  start(port){
    if(port){
      const libc=Process.getModuleByName('libc.so');
      const socket=new NativeFunction(libc.getExportByName('socket'),'int',['int','int','int']);
      const connect=new NativeFunction(libc.getExportByName('connect'),'int',['int','pointer','int']);
      const fd=socket(2,1,0), address=Memory.alloc(16);
      address.writeU16(2);address.add(2).writeU8(port>>8);address.add(3).writeU8(port&255);
      address.add(4).writeByteArray([127,0,0,1]);
      if(fd<0||connect(fd,address,16)!==0||configure(fd)!==0)throw new Error('companion connection failed');
    }
    const getVM=new NativeFunction(Process.getModuleByName('libart.so').getExportByName('JNI_GetCreatedJavaVMs'), 'int',['pointer','int','pointer']);
    const vm=Memory.alloc(8), count=Memory.alloc(4);
    if(getVM(vm,1,count)!==0||count.readS32()!==1)throw new Error('JavaVM unavailable');
    return start(vm.readPointer());
  },
  status(){return status();},
  stop(){if(stop)stop();},
  close(){if(scene&&!scene.isNull()){close(scene);scene=NULL;}}
};
'''
device = frida.get_device_manager().add_remote_device('127.0.0.1:27043')
session = device.attach(a.pid)
script = session.create_script(source)
script.load()
report = {'inputSeconds': a.input_seconds, 'samples': []}
def save():
    pathlib.Path(a.output).write_text(json.dumps(report, indent=2), encoding='utf-8')
def detached(reason, crash=None):
    report['detachReason'] = reason
    report['crash'] = str(crash) if crash else None
    save()
session.on('detached', detached)
started = False
try:
    report['init'] = script.exports_sync.init(a.device_library, symbols)
    for _ in range(20):
        report['samples'].append(script.exports_sync.snapshot())
        save()
        time.sleep(.025)
    if a.input_seconds:
        started = True
        report['startResult'] = script.exports_sync.start(a.companion_port)
        deadline = time.monotonic()+a.input_seconds
        while time.monotonic() < deadline:
            sample = script.exports_sync.snapshot()
            sample['status'] = script.exports_sync.status()
            report['samples'].append(sample)
            save()
            time.sleep(.1)
except Exception as e:
    report['error'] = str(e)
finally:
    if started:
        try:
            script.exports_sync.stop()
            time.sleep(.3)
            report['stoppedStatus'] = script.exports_sync.status()
        except Exception as e: report['stopError'] = str(e)
    try: script.exports_sync.close()
    except Exception as e: report['closeError'] = str(e)
    try: session.detach()
    except Exception: pass
    # The library remains mapped if Java natives were registered. Never dlclose it.
    save()
ready = [s for s in report['samples'] if s.get('ready')]
print(json.dumps({'error': report.get('error'), 'readySamples':len(ready), 'samples':len(report['samples']),
                  'first':ready[0]['candidates'][:1] if ready else None,'last':ready[-1]['candidates'][:1] if ready else None,
                  'statuses':sorted({s['status'] for s in report['samples'] if 'status' in s})},indent=2))
