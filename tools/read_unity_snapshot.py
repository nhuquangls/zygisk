"""Read verified native Unity Transform TRS hierarchy and cached camera matrices.

Diagnostic for the inspected libunity build only. All addresses of objects are
from the supplied capture and checked against the current process. No getters.
"""
import argparse
import json
from pathlib import Path
import frida

SCRIPT = r'''
rpc.exports = {
  read(metadata) {
    verifyUnityProfile();
    if (Process.id !== metadata.pid || Process.getModuleByName('libil2cpp.so').base.toString() !== metadata.base)
      throw new Error('Capture belongs to another process');
    const u32 = p => p.readU32();
    const vec = (p, n=3) => Array.from({length:n}, (_,i)=>p.add(i*4).readFloat());
    const cross = (a,b) => [a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]];
    function position(native) {
      const data = native.add(0x40).readPointer(), index = u32(native.add(0x48));
      if (data.isNull() || index >= 65536) throw new Error('Invalid Transform data/index');
      const trs = data.add(8).readPointer(), parents = data.add(16).readPointer();
      if (trs.isNull() || parents.isNull()) throw new Error('Invalid Transform arrays');
      let value = vec(trs.add(index*40)), parent = parents.add(index*4).readS32();
      const seen = new Set([index]);
      const chain = [];
      while (parent >= 0) {
        if (parent >= 65536 || seen.has(parent) || chain.length >= 128) throw new Error('Invalid Transform ancestry');
        seen.add(parent);chain.push(parent);
        const p = trs.add(parent*40), t = vec(p), q = vec(p.add(12),4), scale = vec(p.add(28));
        if (Math.abs(q.reduce((s,v)=>s+v*v,0)-1)>0.01) throw new Error('Non-unit Transform quaternion');
        const v = value.map((x,i)=>x*scale[i]), c = cross(q,v), cc = cross(q,c);
        value = v.map((x,i)=>x+2*q[3]*c[i]+2*cc[i]+t[i]);
        parent = parents.add(parent*4).readS32();
      }
      if (parent !== -1 || !value.every(Number.isFinite) || !native.add(0x40).readPointer().equals(data) || u32(native.add(0x48))!==index)
        throw new Error('Invalid/changed Transform');
      return {world:value,index,chain};
    }
    const result = {pid:Process.id, capturedAt:new Date().toISOString(), camera:[], pawns:[]};
    const il2cpp=Process.getModuleByName('libil2cpp.so');
    const staticGet=new NativeFunction(il2cpp.getExportByName('il2cpp_field_static_get_value'),'void',['pointer','pointer']);
    const cameraClass=metadata.classes.find(c=>c.namespace==='WNEngine'&&c.name==='PlayerCamera');
    function cameraStatic(name, kind) {
      const field=cameraClass.fields.find(f=>f.name===name&&(f.flags&0x10)!==0), slot=Memory.alloc(16);
      if(!field)throw new Error('Missing camera static '+name);
      staticGet(ptr(field.address),slot);
      return kind==='vector'?vec(slot):kind==='int'?slot.readS32():kind==='pointer'?slot.readPointer().toString():slot.readFloat();
    }
    result.frameBefore=cameraStatic('LastWorldCameraUpdateFrame','int');
    result.cameraPosition=cameraStatic('m_WorldCameraPosition','vector');
    result.cameraForward=cameraStatic('m_WorldCameraForwardDir','vector');
    result.cameraFov=cameraStatic('m_WorldCameraFOV','float');
    const cached = metadata.classes.find(c=>c.namespace==='UnityEngine'&&c.name==='Object').fields.find(f=>f.name==='m_CachedPtr');
    for (const field of metadata.staticFields.filter(f=>f.class==='PlayerCamera'&&['m_WorldCamera','m_FirstPersonCamera'].includes(f.name))) {
      try {
        const object=ptr(cameraStatic(field.name,'pointer')), native=object.add(Number(cached.offset)).readPointer();
        const flags=Array.from({length:8},(_,i)=>native.add(0x520+i).readU8());
        result.camera.push({name:field.name,object:object.toString(),native:native.toString(),flags,
                            view:vec(native.add(0x48),16),projection:vec(native.add(0x88),16)});
      } catch(error) {result.camera.push({name:field.name,error:String(error)});}
    }
    const pawnLayout = metadata.classes.find(c=>c.namespace==='WNEngine'&&c.name==='Pawn');
    for (const root of metadata.roots) for(const sample of root.samples || []) {
      try {
        const object=ptr(sample.address), native=ptr(sample.nativeTransform);
        const p={slot:sample.slot, address:sample.address, camp:sample.camp, isLocal:sample.isLocalPawn===true,
                 visible:sample.pawnVisible,position:position(native), transforms:{}, dimensions:{}};
        for (const f of pawnLayout.fields.filter(f=>f.type==='UnityEngine.Transform'&&(f.flags&0x10)===0)) {
          const transform=object.add(Number(f.offset)).readPointer();
          if(!transform.isNull()) {
            try {p.transforms[f.name]=position(transform.add(Number(cached.offset)).readPointer());}
            catch(error){p.transforms[f.name]={error:String(error)};}
          }
        }
        for (const f of pawnLayout.fields.filter(f=>['m_StandingEyeHeight','m_StandingColliderHeight'].includes(f.name)))
          p.dimensions[f.name]=object.add(Number(f.offset)).readFloat();
        result.pawns.push(p);
      } catch(error) {result.pawns.push({slot:sample.slot,error:String(error)});}
    }
    result.frameAfter=cameraStatic('LastWorldCameraUpdateFrame','int');
    result.sameCameraFrame=result.frameBefore===result.frameAfter;
    return result;
  }
};
'''

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--metadata', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    data = json.loads(args.metadata.read_text(encoding='utf-8'))
    session = frida.get_device_manager().add_remote_device('127.0.0.1:27043').attach(data['pid'])
    try:
        script = session.create_script(Path(__file__).with_name('unity_profile.js').read_text(encoding='utf-8') + '\n' + SCRIPT)
        script.load()
        result = script.exports_sync.read(data)
    finally:
        session.detach()
    args.output.write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(json.dumps(result, indent=2))

if __name__ == '__main__':
    main()
