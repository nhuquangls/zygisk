'use strict';

// Diagnostic for the libunity layout inspected in this session. Copies game
// state only; no Unity/game getters, field setters, or hooks.
rpc.exports = {
  snapshot(metadata, width, height) {
    verifyUnityProfile();
    const mod = Process.getModuleByName('libil2cpp.so');
    if (Process.id !== metadata.pid || mod.base.toString() !== metadata.base)
      throw new Error('Process changed; collect metadata again');
    const klass = (ns, name) => metadata.classes.find(c => c.namespace === ns && c.name === name);
    const field = (layout, name) => {
      const f = layout.fields.find(f => f.name === name);
      if (!f) throw new Error('Missing field ' + layout.name + '.' + name);
      return f;
    };
    const at = (object, layout, name) => object.add(Number(field(layout, name).offset));
    const statics = new NativeFunction(mod.getExportByName('il2cpp_field_static_get_value'), 'void', ['pointer', 'pointer']);
    function getStatic(layout, name, kind) {
      const f = field(layout, name), p = Memory.alloc(16);
      if (!(f.flags & 0x10)) throw new Error('Expected static field');
      statics(ptr(f.address), p);
      return kind === 'int' ? p.readS32() : kind === 'vector' ? floats(p, 3) : p.readPointer();
    }
    function floats(address, count) {
      const bytes = new DataView(address.readByteArray(count * 4));
      const result = Array.from({length:count}, (_, i) => bytes.getFloat32(i * 4, true));
      if (!result.every(Number.isFinite)) throw new Error('Non-finite vector/matrix');
      return result;
    }
    const cross = (a,b) => [a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]];
    const unityObject = klass('UnityEngine', 'Object');
    function world(transform) {
      const native = at(transform, unityObject, 'm_CachedPtr').readPointer();
      if (native.isNull()) throw new Error('Destroyed Transform');
      const data = native.add(0x40).readPointer(), index = native.add(0x48).readU32();
      if (data.isNull() || index >= 65536) throw new Error('Invalid Transform index');
      const trs = data.add(8).readPointer(), parents = data.add(16).readPointer();
      let value = floats(trs.add(index * 40), 3), parent = parents.add(index * 4).readS32();
      const seen = new Set([index]);
      while (parent >= 0) {
        if (parent >= 65536 || seen.has(parent) || seen.size > 128) throw new Error('Invalid parent chain');
        seen.add(parent);
        const row = floats(trs.add(parent * 40), 10), q = row.slice(3,7);
        if (Math.abs(q.reduce((s,x)=>s+x*x,0)-1)>0.01) throw new Error('Invalid rotation');
        const v=value.map((x,i)=>x*row[i+7]), a=cross(q,v), b=cross(q,a);
        value=v.map((x,i)=>row[i]+x+2*q[3]*a[i]+2*b[i]);
        parent=parents.add(parent*4).readS32();
      }
      if(parent!==-1 || !value.every(Number.isFinite)) throw new Error('Invalid world position');
      return value;
    }
    const managerClass=klass('WNEngine','AttackableTargetsManager'), cameraClass=klass('WNEngine','PlayerCamera');
    const pawn=klass('WNEngine','Pawn'), wnPawn=klass('WNGameBase','WNPawn'), target=klass('WNEngine','AttackableTarget');
    const pc=klass('WNEngine','PlayerController'), controller=klass('WNEngine','Controller');
    const localPlayer=klass('WNEngine','LocalPlayer'), playerInfo=klass('WNEngine','PlayerInfo');
    const component=klass('WNEngine','BaseComponent');
    const manager=getStatic(managerClass,'_Instance','pointer');
    if(manager.isNull())return {ready:false,reason:'no manager'};
    const list=at(manager,managerClass,'m_AttackableTargetList').readPointer();
    if(list.isNull())return {ready:false,reason:'no list'};
    const actualList=metadata.classes.find(c=>c.address===list.readPointer().toString());
    const layout=actualList && metadata.classes.find(c=>c.address===actualList.parent.address);
    if(!layout)return {ready:false,reason:'unknown list class'};
    const count=at(list,layout,'_size').readS32(), version=at(list,layout,'_version').readS32();
    const items=at(list,layout,'_items').readPointer();
    if(count<=0 || count>256 || items.isNull() || items.add(24).readU64().toNumber()<count)
      return {ready:false,reason:'empty/invalid list'};
    const camera=getStatic(cameraClass,'m_WorldCamera','pointer');
    if(camera.isNull())return {ready:false,reason:'no world camera'};
    const nativeCamera=at(camera,unityObject,'m_CachedPtr').readPointer();
    if(nativeCamera.isNull())return {ready:false,reason:'destroyed camera'};
    const frame=getStatic(cameraClass,'LastWorldCameraUpdateFrame','int');
    const cameraPosition=getStatic(cameraClass,'m_WorldCameraPosition','vector');
    const cameraForward=getStatic(cameraClass,'m_WorldCameraForwardDir','vector');
    const view=floats(nativeCamera.add(0x48),16), projection=floats(nativeCamera.add(0x88),16);
    const mul=(m,x)=>Array.from({length:4},(_,row)=>x.reduce((s,v,col)=>s+m[col*4+row]*v,0));
    const origin=mul(view,[...cameraPosition,1]);
    if(Math.hypot(...origin.slice(0,3))>0.1 ||
       Math.hypot(view[2]+cameraForward[0],view[6]+cameraForward[1],view[10]+cameraForward[2])>0.02)
      return {ready:false,reason:'camera cache differs from current pose'};
    if(projection[0]<=0 || projection[5]<=0 || Math.abs(projection[11]+1)>1e-5 || Math.abs(projection[15])>1e-5)
      return {ready:false,reason:'unsupported projection'};
    const allowed=new Set();
    for(const c of metadata.classes){
      let current=c;
      for(let i=0;current&&i<16;i++){
        if(current.address===wnPawn.address){allowed.add(c.address);break;}
        current=current.parent&&metadata.classes.find(x=>x.address===current.parent.address);
      }
    }
    const pawns=[];let local=null;
    for(let i=0;i<count;i++){
      try{
        const object=items.add(32+i*8).readPointer();
        if(object.isNull() || !allowed.has(object.readPointer().toString()))continue;
        if(at(object,component,'bHasDestroy').readU8())continue;
        const info=at(object,pawn,'m_PlayerInfo').readPointer();
        if(info.isNull())continue;
        const entry={slot:i,address:object.toString(),camp:at(info,playerInfo,'m_Camp').readS32(),
                     health:at(object,target,'m_Health').readFloat(),hidden:at(object,target,'m_IsHidden').readU8(),
                     visible:at(object,wnPawn,'m_IsPawnVisible').readU8()};
        const ctrl=at(object,pawn,'m_Controller').readPointer();
        if(!ctrl.isNull() && at(ctrl,controller,'m_Pawn').readPointer().equals(object)){
          const lp=at(ctrl,pc,'m_LocalPlayer').readPointer();
          if(!lp.isNull() && at(lp,localPlayer,'m_PlayerController').readPointer().equals(ctrl)){
            entry.aimAssist={
              enabled:at(ctrl,pc,'m_EnableAimAssistance').readU8(),
              sniperEnabled:at(ctrl,pc,'m_EnableAimAssistanceForSniper').readU8(),
              doing:at(ctrl,pc,'m_DoingAimAssist').readU8()
            };
            local=entry;
          }
        }
        const upper=at(object,pawn,'m_CachedUpperBodyTransform').readPointer();
        if(!upper.isNull()){
          entry.point=world(upper);
          const clip=mul(projection,mul(view,[...entry.point,1]));
          if(clip[3]>0.01){
            entry.screen=[(clip[0]/clip[3]+1)*width/2,(1-clip[1]/clip[3])*height/2];
            entry.error=[entry.screen[0]-width/2,entry.screen[1]-height/2];
            entry.distance=Math.hypot(...entry.error);
          }
        }
        pawns.push(entry);
      }catch(error){ /* Discard a raced/destroyed object. */ }
    }
    const frameAfter=getStatic(cameraClass,'LastWorldCameraUpdateFrame','int');
    const viewAfter=floats(nativeCamera.add(0x48),16), projectionAfter=floats(nativeCamera.add(0x88),16);
    if(frameAfter!==frame || view.some((v,i)=>v!==viewAfter[i]) || projection.some((v,i)=>v!==projectionAfter[i]) ||
       at(list,layout,'_size').readS32()!==count || at(list,layout,'_version').readS32()!==version ||
       !at(list,layout,'_items').readPointer().equals(items))return {ready:false,reason:'frame/list changed'};
    if(!local || !(local.health>0) || ![1,2].includes(local.camp))return {ready:false,reason:'local pawn unavailable'};
    const candidates=pawns.filter(p=>p.address!==local.address&&p.camp!==local.camp&&p.health>0&&!p.hidden&&p.visible&&p.screen&&
                                      p.screen[0]>=0&&p.screen[0]<=width&&p.screen[1]>=0&&p.screen[1]<=height);
    candidates.sort((a,b)=>a.distance-b.distance);
    return {ready:true,frame,cameraPosition,cameraForward,width,height,local,candidates,
            fovY:2*Math.atan(1/projection[5])*180/Math.PI};
  }
};
