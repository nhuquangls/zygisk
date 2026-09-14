'use strict';
// Live aim survey: find Transform positions, Camera matrix, and scan all Pawn
// fields for non-zero data. Run while game is in a match.
//
// Usage: frida -U -p <PID> -l live_aim_survey.js --no-pause
rpc.exports = {
  survey() {
    const mod = Process.getModuleByName('libil2cpp.so');
    const exports = new Map(mod.enumerateExports().map(e => [e.name, e]));

    function api(name, ret, args) {
      const s = exports.get('il2cpp_' + name);
      if (!s) throw new Error('Missing: il2cpp_' + name);
      return new NativeFunction(s.address, ret, args || []);
    }

    const domainGet = api('domain_get', 'pointer');
    const assembliesGet = api('domain_get_assemblies', 'pointer', ['pointer', 'pointer']);
    const assemblyImage = api('assembly_get_image', 'pointer', ['pointer']);
    const imageName = api('image_get_name', 'pointer', ['pointer']);
    const classFromName = api('class_from_name', 'pointer', ['pointer', 'pointer', 'pointer']);
    const classSize = api('class_instance_size', 'uint32', ['pointer']);
    const classFields = api('class_get_fields', 'pointer', ['pointer', 'pointer']);
    const fieldName = api('field_get_name', 'pointer', ['pointer']);
    const fieldType = api('field_get_type', 'pointer', ['pointer']);
    const fieldOffset = api('field_get_offset', 'uint64', ['pointer']);
    const fieldFlags = api('field_get_flags', 'int', ['pointer']);
    const fieldStaticGet = api('field_static_get_value', 'void', ['pointer', 'pointer']);
    const objectClass = api('object_get_class', 'pointer', ['pointer']);
    const isAssignable = api('class_is_assignable_from', 'bool', ['pointer', 'pointer']);
    const className = api('class_get_name', 'pointer', ['pointer']);
    const classNamespace = api('class_get_namespace', 'pointer', ['pointer']);
    const typeName = api('type_get_name', 'pointer', ['pointer']);
    const typeKind = api('type_get_type', 'int', ['pointer']);
    const free = api('free', 'void', ['pointer']);
    const threadCurrent = api('thread_current', 'pointer');
    const threadAttach = api('thread_attach', 'pointer', ['pointer']);
    const threadDetach = api('thread_detach', 'void', ['pointer']);

    const str = p => p.isNull() ? null : p.readUtf8String();

    const domain = domainGet();
    if (domain.isNull()) throw new Error('No IL2CPP domain');
    let attached = null;
    if (threadCurrent().isNull()) attached = threadAttach(domain);

    const output = { pid: Process.id, capturedAt: new Date().toISOString(), sections: {} };

    try {
      // Enumerate assemblies
      const countBuf = Memory.alloc(8).writeU64(0);
      const list = assembliesGet(domain, countBuf);
      const count = countBuf.readU64().toNumber();
      const images = new Map();
      for (let i = 0; i < count; i++) {
        const img = assemblyImage(list.add(i * 8).readPointer());
        const nm = str(imageName(img));
        images.set(nm, img);
      }

      function getClass(imageName, ns, name) {
        const img = images.get(imageName);
        if (!img) return null;
        return classFromName(img, Memory.allocUtf8String(ns || ''), Memory.allocUtf8String(name));
      }

      // Helper: get all instance fields of a class (including parents)
      function getAllFields(klass) {
        const fields = [];
        let cur = klass;
        while (cur && !cur.isNull()) {
          const iter = Memory.alloc(8).writePointer(NULL);
          for (let i = 0; i < 4096; i++) {
            const f = classFields(cur, iter);
            if (f.isNull()) break;
            fields.push({
              name: str(fieldName(f)),
              offset: Number(fieldOffset(f)),
              flags: fieldFlags(f),
              type: str(typeName(fieldType(f))),
              kind: typeKind(fieldType(f)),
            });
          }
          const p = classParent(cur);
          cur = (p && !p.isNull()) ? p : null;
        }
        return fields;
      }

      function readField(object, field) {
        const addr = object.add(field.offset);
        switch (field.kind) {
          case 2: case 3: return addr.readU8();           // bool/byte
          case 4: case 5: return addr.readS16();          // short
          case 6: case 7: return addr.readS32();          // int/enum
          case 8: case 9: case 10: return addr.readS64(); // long
          case 11: return addr.readFloat();
          case 12: return addr.readDouble();
          case 13: return addr.readS32(); // char
          case 14: case 15: return addr.readPointer().toString(); // string/ptr
          case 16: case 17: case 18: case 21: case 28: case 29: case 30: case 39:
            return addr.readPointer().toString();
          default: return addr.readPointer().toString();
        }
      }

      // ========== SECTION 1: Get AttackableTargetsManager + list ==========
      const managerClass = getClass('Assembly-CSharp.dll', 'WNEngine', 'AttackableTargetsManager');
      if (!managerClass || managerClass.isNull()) throw new Error('No AttackableTargetsManager');

      const managerFields = getAllFields(managerClass);
      const instanceField = managerFields.find(f => f.name === '_Instance' && (f.flags & 0x10) !== 0);
      if (!instanceField) throw new Error('No _Instance field');

      const instancePtr = Memory.alloc(8);
      instancePtr.writePointer(NULL);
      fieldStaticGet(instanceField, instancePtr);
      const managerInstance = instancePtr.readPointer();
      if (managerInstance.isNull()) throw new Error('Manager instance is null');

      const listField = managerFields.find(f => f.name === 'm_AttackableTargetList');
      const listPtr = managerInstance.add(listField.offset).readPointer();
      if (listPtr.isNull()) throw new Error('Target list is null');

      // Read list container fields
      const listClass = objectClass(listPtr);
      const listFields = getAllFields(listClass);
      const itemsField = listFields.find(f => f.name === '_items');
      const sizeField = listFields.find(f => f.name === '_size');
      const items = listPtr.add(itemsField.offset).readPointer();
      const size = listPtr.add(sizeField.offset).readS32();
      output.sections.listInfo = { size, listAddress: listPtr.toString(), itemsAddress: items.toString() };

      // ========== SECTION 2: Scan ALL Pawn fields for non-zero data ==========
      const pawnClass = getClass('Assembly-CSharp.dll', 'WNEngine', 'Pawn');
      const pawnFields = getAllFields(pawnClass);

      // Read first pawn object to get class
      const firstPawnObj = items.add(32).readPointer(); // ARM64 array header = 32 bytes
      const pawnItems = [];
      const toRead = Math.min(size, 15); // limit

      for (let i = 0; i < toRead; i++) {
        const obj = items.add(32 + i * 8).readPointer();
        if (obj.isNull()) continue;
        const objClass = objectClass(obj);
        if (!isAssignable(pawnClass, objClass)) continue;

        const sample = { slot: i, address: obj.toString(), class: str(className(objClass)), fields: {} };

        // Read EVERY field and record non-zero/non-null ones
        for (const f of pawnFields) {
          if (f.flags & 0x20) continue; // skip static
          try {
            const val = readField(obj, f);
            // Check if meaningful
            let meaningful = false;
            if (typeof val === 'string') {
              meaningful = val !== '0x0' && val !== '0';
            } else if (typeof val === 'number') {
              meaningful = val !== 0;
            } else {
              meaningful = val !== 0;
            }
            if (meaningful) {
              sample.fields[f.name] = { offset: f.offset, kind: f.kind, type: f.type, value: val };
            }
          } catch (e) {
            sample.fields[f.name] = { offset: f.offset, error: String(e) };
          }
        }
        pawnItems.push(sample);
      }
      output.sections.pawnFieldScan = pawnItems;

      // ========== SECTION 3: Read Transform native position ==========
      const baseObjectClass = getClass('Assembly-CSharp.dll', 'WNCore', 'BaseObject');
      const baseObjectFields = getAllFields(baseObjectClass);
      const transformField = baseObjectFields.find(f => f.name === 'm_Transform');
      const unityObjClass = getClass('UnityEngine.dll', 'UnityEngine', 'Object');
      const unityObjFields = unityObjFields || getAllFields(unityObjClass);
      const cachedPtrField = unityObjFields.find(f => f.name === 'm_CachedPtr');

      // Get Transform class to find SetPosition/GetPosition
      const transformClass = getClass('UnityEngine.dll', 'UnityEngine', 'Transform');
      let getLocalPositionAddr = null;
      let getPositionAddr = null;
      if (transformClass && !transformClass.isNull()) {
        const iter = Memory.alloc(8).writePointer(NULL);
        for (let i = 0; i < 4096; i++) {
          const m = classMethods(transformClass, iter);
          if (m.isNull()) break;
          const mName = str(methodName(m));
          if (mName === 'get_position') getPositionAddr = m.readPointer();
          if (mName === 'get_localPosition') getLocalPositionAddr = m.readPointer();
        }
      }

      const transformPositions = [];
      for (const sample of pawnItems) {
        const obj = ptr(sample.address);
        if (transformField && cachedPtrField) {
          const transformObj = obj.add(transformField.offset).readPointer();
          if (!transformObj.isNull()) {
            const nativePtr = transformObj.add(cachedPtrField.offset).readPointer();
            if (!nativePtr.isNull() && getPositionAddr && !getPositionAddr.isNull()) {
              try {
                const getPos = new NativeFunction(getPositionAddr, 'void', ['pointer', 'pointer']);
                const result = Memory.alloc(12);
                getPos(nativePtr, result);
                const pos = [result.readFloat(), result.add(4).readFloat(), result.add(8).readFloat()];
                transformPositions.push({ slot: sample.slot, address: sample.address, nativeTransform: nativePtr.toString(), position: pos });
              } catch (e) {
                transformPositions.push({ slot: sample.slot, error: String(e) });
              }
            }
          }
        }
      }
      output.sections.transformPositions = transformPositions;

      // ========== SECTION 4: Get Camera matrices ==========
      const cameraClass = getClass('UnityEngine.dll', 'UnityEngine', 'Camera');
      const cameraResults = {};
      if (cameraClass && !cameraClass.isNull()) {
        // Find Camera.main
        const iter = Memory.alloc(8).writePointer(NULL);
        let mainMethod = null, worldToCameraMethod = null, projMethod = null;
        let fieldList = [];
        for (let i = 0; i < 4096; i++) {
          const m = classMethods(cameraClass, iter);
          if (m.isNull()) break;
          const mName = str(methodName(m));
          if (mName === 'get_main') mainMethod = m.readPointer();
          if (mName === 'get_worldToCameraMatrix') worldToCameraMethod = m.readPointer();
          if (mName === 'get_projectionMatrix') projMethod = m.readPointer();
        }
        // Get Camera fields
        const camFields = getAllFields(cameraClass);
        cameraResults.fieldNames = camFields.map(f => f.name);

        if (mainMethod && !mainMethod.isNull()) {
          try {
            const getMain = new NativeFunction(mainMethod, 'pointer', []);
            const mainCam = getMain();
            if (!mainCam.isNull()) {
              cameraResults.mainAddress = mainCam.toString();
              cameraResults.mainClass = str(className(objectClass(mainCam)));

              // Read fields
              cameraResults.mainFields = {};
              for (const f of camFields) {
                if (f.flags & 0x20) continue;
                try {
                  const val = readField(mainCam, f);
                  let meaningful = false;
                  if (typeof val === 'string') meaningful = val !== '0x0' && val !== '0';
                  else if (typeof val === 'number') meaningful = val !== 0;
                  else meaningful = val !== 0;
                  if (meaningful) {
                    cameraResults.mainFields[f.name] = { offset: f.offset, kind: f.kind, type: f.type, value: val };
                  }
                } catch (e) {}
              }

              if (worldToCameraMethod && !worldToCameraMethod.isNull()) {
                try {
                  const getW2C = new NativeFunction(worldToCameraMethod, 'void', ['pointer', 'pointer']);
                  const mat = Memory.alloc(64);
                  getW2C(mainCam, mat);
                  const w2c = [];
                  for (let j = 0; j < 16; j++) w2c.push(mat.add(j * 4).readFloat());
                  cameraResults.worldToCameraMatrix = w2c;
                } catch (e) { cameraResults.worldToCameraError = String(e); }
              }
              if (projMethod && !projMethod.isNull()) {
                try {
                  const getProj = new NativeFunction(projMethod, 'void', ['pointer', 'pointer']);
                  const mat = Memory.alloc(64);
                  getProj(mainCam, mat);
                  const proj = [];
                  for (let j = 0; j < 16; j++) proj.push(mat.add(j * 4).readFloat());
                  cameraResults.projectionMatrix = proj;
                } catch (e) { cameraResults.projectionError = String(e); }
              }
            }
          } catch (e) { cameraResults.mainError = String(e); }
        }
      }
      output.sections.camera = cameraResults;

      // ========== SECTION 5: Check local pawn's controller chain ==========
      const controllerClass = getClass('Assembly-CSharp.dll', 'WNEngine', 'Controller');
      const playerControllerClass = getClass('Assembly-CSharp.dll', 'WNEngine', 'PlayerController');
      const localPlayerClass = getClass('Assembly-CSharp.dll', 'WNEngine', 'LocalPlayer');
      const localPawnInfo = [];
      for (const sample of pawnItems) {
        if (!sample.fields.m_Controller && !sample.fields.m_LocalPlayerController) continue;
        // Already have controller data in pawnFieldScan
        localPawnInfo.push({ slot: sample.slot, hasController: !!sample.fields.m_Controller, hasLocal: !!sample.fields.m_LocalPlayerController });
      }
      output.sections.localPawnInfo = localPawnInfo;

      // ========== SECTION 6: Scan WNPVPGame / PVPPlayerPawn for extra position fields ==========
      const pvpPawnClass = getClass('Assembly-CSharp.dll', 'WNPVPGame', 'PVPPlayerPawn');
      if (pvpPawnClass && !pvpPawnClass.isNull()) {
        const pvpFields = getAllFields(pvpPawnClass);
        output.sections.pvpPawnFieldNames = pvpFields.filter(f => !(f.flags & 0x20)).map(f => ({ name: f.name, offset: f.offset, kind: f.kind }));
        // Read PVP fields from first pawn
        if (pawnItems.length > 0) {
          const obj = ptr(pawnItems[0].address);
          if (isAssignable(pvpPawnClass, objectClass(obj))) {
            const pvpData = {};
            for (const f of pvpFields) {
              if (f.flags & 0x20) continue;
              try {
                const val = readField(obj, f);
                let meaningful = false;
                if (typeof val === 'string') meaningful = val !== '0x0' && val !== '0';
                else if (typeof val === 'number') meaningful = val !== 0;
                else meaningful = val !== 0;
                if (meaningful) pvpData[f.name] = { offset: f.offset, kind: f.kind, value: val };
              } catch (e) {}
            }
            output.sections.pvpPawnData0 = pvpData;
          }
        }
      }

      // ========== SECTION 7: Screen resolution ==========
      const screenClass = getClass('UnityEngine.dll', 'UnityEngine', 'Screen');
      if (screenClass && !screenClass.isNull()) {
        const iter = Memory.alloc(8).writePointer(NULL);
        for (let i = 0; i < 4096; i++) {
          const m = classMethods(screenClass, iter);
          if (m.isNull()) break;
          const mName = str(methodName(m));
          if (mName === 'get_width') {
            try {
              const fn = new NativeFunction(m.readPointer(), 'int', []);
              output.sections.screenWidth = fn();
            } catch (e) {}
          }
          if (mName === 'get_height') {
            try {
              const fn = new NativeFunction(m.readPointer(), 'int', []);
              output.sections.screenHeight = fn();
            } catch (e) {}
          }
        }
      }

    } finally {
      if (attached) threadDetach(attached);
    }
    return output;
  }
};
