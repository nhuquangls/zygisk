'use strict';

// Metadata queries only. No Interceptor, code patch, field setter, game method
// invocation or input injection. Execute after IL2CPP has initialized.
rpc.exports = {
  collect(requests, inspectRoots = false) {
    const mod = Process.getModuleByName('libil2cpp.so');
    const exports = new Map(mod.enumerateExports().map(e => [e.name, e]));
    const addresses = {};
    function api(name, result, args) {
      const symbol = exports.get('il2cpp_' + name);
      if (!symbol || symbol.type !== 'function') throw new Error('Missing API: ' + name);
      const range = Process.findRangeByAddress(symbol.address);
      if (!range || !range.protection.includes('x')) throw new Error('Non-executable API: ' + name);
      addresses[name] = {address: symbol.address.toString(), rva: symbol.address.sub(mod.base).toString()};
      return new NativeFunction(symbol.address, result, args);
    }
    const domainGet = api('domain_get', 'pointer', []);
    const assembliesGet = api('domain_get_assemblies', 'pointer', ['pointer', 'pointer']);
    const assemblyImage = api('assembly_get_image', 'pointer', ['pointer']);
    const imageName = api('image_get_name', 'pointer', ['pointer']);
    const classFromName = api('class_from_name', 'pointer', ['pointer', 'pointer', 'pointer']);
    const className = api('class_get_name', 'pointer', ['pointer']);
    const classNamespace = api('class_get_namespace', 'pointer', ['pointer']);
    const classParent = api('class_get_parent', 'pointer', ['pointer']);
    const classSize = api('class_instance_size', 'uint32', ['pointer']);
    const classFields = api('class_get_fields', 'pointer', ['pointer', 'pointer']);
    const fieldName = api('field_get_name', 'pointer', ['pointer']);
    const fieldType = api('field_get_type', 'pointer', ['pointer']);
    const fieldOffset = api('field_get_offset', 'uint64', ['pointer']);
    const fieldFlags = api('field_get_flags', 'int', ['pointer']);
    const fieldStaticGet = api('field_static_get_value', 'void', ['pointer', 'pointer']);
    const objectClass = api('object_get_class', 'pointer', ['pointer']);
    const isAssignable = api('class_is_assignable_from', 'bool', ['pointer', 'pointer']);
    const arrayLength = api('array_length', 'uint64', ['pointer']);
    const arrayElementSize = api('array_element_size', 'int', ['pointer']);
    const typeName = api('type_get_name', 'pointer', ['pointer']);
    const typeKind = api('type_get_type', 'int', ['pointer']);
    const free = api('free', 'void', ['pointer']);
    const classMethods = api('class_get_methods', 'pointer', ['pointer', 'pointer']);
    const methodName = api('method_get_name', 'pointer', ['pointer']);
    const methodCount = api('method_get_param_count', 'uint32', ['pointer']);
    const threadCurrent = api('thread_current', 'pointer', []);
    const threadAttach = api('thread_attach', 'pointer', ['pointer']);
    const threadDetach = api('thread_detach', 'void', ['pointer']);
    const str = p => p.isNull() ? null : p.readUtf8String();
    const domain = domainGet();
    if (domain.isNull()) throw new Error('IL2CPP domain is not initialized');
    let attached = null;
    if (threadCurrent().isNull()) {
      attached = threadAttach(domain);
      if (attached.isNull()) throw new Error('thread_attach failed');
    }
    const output = {pid: Process.id, base: mod.base.toString(), path: mod.path,
                    capturedAt: new Date().toISOString(),
                    assemblies: [], classes: [], missing: [], roots: [], staticFields: [], apis: addresses};
    try {
      const countBuffer = Memory.alloc(8);
      countBuffer.writeU64(0);
      const list = assembliesGet(domain, countBuffer);
      const count = countBuffer.readU64().toNumber();
      if (list.isNull() || count < 1 || count > 1024) throw new Error('Invalid assembly list');
      const images = new Map();
      for (let i = 0; i < count; ++i) {
        const image = assemblyImage(list.add(i * 8).readPointer());
        const name = str(imageName(image));
        images.set(name, image);
        output.assemblies.push({name, address: image.toString()});
      }
      const seen = new Set();
      function describe(klass) {
        if (klass.isNull() || seen.has(klass.toString())) return;
        seen.add(klass.toString());
        const name = str(className(klass)), namespace = str(classNamespace(klass));
        const record = {name, namespace, address: klass.toString(), size: classSize(klass), fields: [], methods: []};
        const parent = classParent(klass);
        record.parent = parent.isNull() ? null : {name: str(className(parent)), namespace: str(classNamespace(parent)), address: parent.toString()};
        const iterator = Memory.alloc(8);
        iterator.writePointer(NULL);
        for (let i = 0; i < 4096; ++i) {
          const field = classFields(klass, iterator);
          if (field.isNull()) break;
          const type = fieldType(field);
          const typeText = typeName(type);
          let typeString;
          try { typeString = str(typeText); } finally { if (!typeText.isNull()) free(typeText); }
          const fieldLabel = str(fieldName(field));
          record.fields.push({name: fieldLabel, offset: fieldOffset(field).toString(),
                              flags: fieldFlags(field), type: typeString, kind: typeKind(type), address: field.toString()});
          if (inspectRoots && namespace === 'WNEngine' &&
              ['PlayerCamera', 'EWalkingState', 'EPhysicsState'].includes(name) &&
              (fieldFlags(field) & 0x10) !== 0) {
            const slot = Memory.alloc(32);
            fieldStaticGet(field, slot);
            const kind = typeKind(type);
            let value = null;
            if (kind === 18 || kind === 21) value = slot.readPointer().toString();
            else if (kind === 12) value = slot.readFloat();
            else if (kind === 8 || (kind === 17 && name.startsWith('E'))) value = slot.readS32();
            else if (kind === 2) value = slot.readU8();
            else if (typeString === 'UnityEngine.Vector3') value = [slot.readFloat(), slot.add(4).readFloat(), slot.add(8).readFloat()];
            output.staticFields.push({class:name, name:fieldLabel, type:typeString, value});
          }
          if (inspectRoots && name === 'AttackableTargetsManager' && fieldLabel === '_Instance' &&
              (fieldFlags(field) & 0x10) !== 0 && typeKind(type) === 18) {
            const slot = Memory.alloc(8);
            slot.writePointer(NULL);
            fieldStaticGet(field, slot);
            output.roots.push({class: name, field: fieldLabel, address: slot.readPointer().toString()});
          }
          if (i === 4095) throw new Error('Field enumeration limit: ' + name);
        }
        iterator.writePointer(NULL);
        for (let i = 0; i < 4096; ++i) {
          const method = classMethods(klass, iterator);
          if (method.isNull()) break;
          const pointer = method.readPointer();
          record.methods.push({name: str(methodName(method)), params: methodCount(method),
                               address: pointer.toString(), rva: pointer.isNull() ? null : pointer.sub(mod.base).toString()});
          if (i === 4095) throw new Error('Method enumeration limit: ' + name);
        }
        output.classes.push(record);
        describe(parent);
      }
      for (const request of requests) {
        const image = images.get(request.image || 'Assembly-CSharp.dll');
        if (!image) { output.missing.push({...request, reason: 'image missing'}); continue; }
        const klass = classFromName(image, Memory.allocUtf8String(request.namespace || ''), Memory.allocUtf8String(request.name));
        if (klass.isNull()) output.missing.push({...request, reason: 'class missing'});
        else describe(klass);
      }
      // Read only the manager's list reference. Container layout is obtained
      // from the concrete object's runtime class, not assumed List<T> offsets.
      for (const root of output.roots) {
        const instance = ptr(root.address);
        if (instance.isNull()) continue;
        const managerClass = objectClass(instance);
        const manager = output.classes.find(c => c.address === managerClass.toString());
        const listField = manager.fields.find(f => f.name === 'm_AttackableTargetList');
        if (!listField || listField.kind !== 21) continue;
        const list = instance.add(Number(listField.offset)).readPointer();
        root.list = list.toString();
        if (list.isNull()) continue;
        const listClass = objectClass(list);
        describe(listClass);
        root.listClass = listClass.toString();
        const layouts = [];
        for (let address = root.listClass; address;) {
          const layout = output.classes.find(c => c.address === address);
          if (!layout) break;
          layouts.push(layout);
          address = layout.parent ? layout.parent.address : null;
        }
        root.container = [];
        for (const field of layouts.flatMap(c => c.fields)) {
          if ((field.flags & 0x10) !== 0) continue;
          const location = list.add(Number(field.offset));
          let value = null;
          if (field.kind === 8) value = location.readS32();
          else if ([18, 21, 28, 29].includes(field.kind)) value = location.readPointer().toString();
          root.container.push({name:field.name, offset:field.offset, type:field.type, kind:field.kind, value});
        }
        const itemField = root.container.find(f => f.name === '_items' && f.kind === 29);
        const sizeField = root.container.find(f => f.name === '_size' && f.kind === 8);
        const versionField = root.container.find(f => f.name === '_version' && f.kind === 8);
        if (!itemField || !sizeField || !versionField || sizeField.value < 0 || sizeField.value > 256) continue;
        const items = ptr(itemField.value);
        if (items.isNull()) continue;
        const capacity = arrayLength(items).toNumber();
        const stride = arrayElementSize(objectClass(items));
        root.capacity = capacity;
        root.elementSize = stride;
        if (capacity < sizeField.value || capacity > 65536 || stride !== 8) continue;
        // Standard ARM64 Il2CppArray header is 32 bytes. Validate each slot's
        // runtime class before reading Pawn fields; this is a diagnostic sample.
        const pawn = output.classes.find(c => c.namespace === 'WNEngine' && c.name === 'Pawn');
        const target = output.classes.find(c => c.namespace === 'WNEngine' && c.name === 'AttackableTarget');
        const baseComp = output.classes.find(c => c.namespace === 'WNEngine' && c.name === 'BaseComponent');
        const baseObject = output.classes.find(c => c.namespace === 'WNCore' && c.name === 'BaseObject');
        const playerController = output.classes.find(c => c.namespace === 'WNEngine' && c.name === 'PlayerController');
        const controller = output.classes.find(c => c.namespace === 'WNEngine' && c.name === 'Controller');
        const playerInfo = output.classes.find(c => c.namespace === 'WNEngine' && c.name === 'PlayerInfo');
        const localPlayerLayout = output.classes.find(c => c.namespace === 'WNEngine' && c.name === 'LocalPlayer');
        const unityObject = output.classes.find(c => c.namespace === 'UnityEngine' && c.name === 'Object');
        if (!pawn || !target || !baseComp || !baseObject || !playerController || !controller) continue;
        function fieldAt(object, layout, name) {
          const field = layout.fields.find(f => f.name === name && (f.flags & 0x10) === 0);
          if (!field) throw new Error('Missing instance field: ' + name);
          return object.add(Number(field.offset));
        }
        function vector(address) { return [address.readFloat(), address.add(4).readFloat(), address.add(8).readFloat()]; }
        root.samples = [];
        for (let i = 0; i < sizeField.value; ++i) {
          try {
            const object = items.add(32 + i * stride).readPointer();
            if (object.isNull()) continue;
            const klass = objectClass(object);
            if (!isAssignable(ptr(pawn.address), klass)) continue;
            describe(klass);
            const wnPawn = output.classes.find(c => c.namespace === 'WNGameBase' && c.name === 'WNPawn');
            const pvpPawn = output.classes.find(c => c.namespace === 'WNPVPGame' && c.name === 'PVPPlayerPawn');
            const health = fieldAt(object, target, 'm_Health').readFloat();
            const localController = fieldAt(object, pawn, 'm_LocalPlayerController').readPointer();
            const sample = {slot:i, class:str(className(klass)), address:object.toString(), health,
                hidden:fieldAt(object, target, 'm_IsHidden').readU8(),
                destroyed:fieldAt(object, baseComp, 'bHasDestroy').readU8(),
                head:vector(fieldAt(object, pawn, 'HeadCharacterPosition')),
                foot:vector(fieldAt(object, pawn, 'FootCharacterPosition')),
                lastPosition:vector(fieldAt(object, pawn, 'LastPlayerPosition')),
                headTransform:fieldAt(object, pawn, 'm_HeadTransform').readPointer().toString(),
                camera:fieldAt(object, pawn, 'm_FirstPersonCamera').readPointer().toString(),
                localController:localController.toString()};
            sample.movement = {
              walkingState:fieldAt(object, pawn, 'm_CurrentWalkingState').readS32(),
              physicsState:fieldAt(object, pawn, 'm_PhysicsState').readS32(),
              actualVelocity:vector(fieldAt(object, pawn, 'm_ActualWalkingSwimmingVelocity')),
              lastSimulatedVelocity:vector(fieldAt(object, pawn, 'm_LastSimulateVelocity')),
              velocityObject:fieldAt(object, pawn, 'm_Velocity').readPointer().toString()
            };
            if (wnPawn && isAssignable(ptr(wnPawn.address), klass)) {
              sample.pawnVisible = fieldAt(object, wnPawn, 'm_IsPawnVisible').readU8();
              sample.lastServerPosition = vector(fieldAt(object, wnPawn, 'm_LastServerMovePos'));
              sample.animationVelocity = vector(fieldAt(object, wnPawn, 'm_AnimationVelocity'));
            }
            if (pvpPawn && isAssignable(ptr(pvpPawn.address), klass)) {
              sample.lastMoveDataPosition = vector(fieldAt(object, pvpPawn, 'm_LastMoveDataPos'));
              sample.outlineVisible = fieldAt(object, pvpPawn, 'm_IsPlayerVisibleForOutline').readU8();
              sample.enemyOutlineVisible = fieldAt(object, pvpPawn, 'm_IsEnemyVisibleForOutline').readS32();
            }
            sample.references = {};
            for (const [layout, name] of [[baseObject, 'm_Transform'], [baseComp, 'm_Root'],
                                         [baseComp, 'm_Game'], [pawn, 'm_Controller'],
                                         [pawn, 'm_PlayerInfo']]) {
              const reference = fieldAt(object, layout, name).readPointer();
              const entry = {address:reference.toString()};
              if (!reference.isNull()) {
                const referenceClass = objectClass(reference);
                describe(referenceClass);
                entry.class = str(className(referenceClass));
                entry.classAddress = referenceClass.toString();
              }
              sample.references[name] = entry;
            }
            const transform = ptr(sample.references.m_Transform.address);
            if (!transform.isNull() && unityObject) {
              sample.nativeTransform = fieldAt(transform, unityObject, 'm_CachedPtr').readPointer().toString();
            }
            const info = ptr(sample.references.m_PlayerInfo.address);
            if (!info.isNull() && playerInfo) sample.camp = fieldAt(info, playerInfo, 'm_Camp').readS32();
            const pawnController = ptr(sample.references.m_Controller.address);
            if (!pawnController.isNull() && isAssignable(ptr(playerController.address), objectClass(pawnController))) {
              sample.controllerPawn = fieldAt(pawnController, controller, 'm_Pawn').readPointer().toString();
              const localPlayer = fieldAt(pawnController, playerController, 'm_LocalPlayer').readPointer();
              sample.controllerLocalPlayer = localPlayer.toString();
              if (!localPlayer.isNull() && localPlayerLayout) {
                sample.localCamp = fieldAt(localPlayer, localPlayerLayout, 'm_Camp').readS32();
                sample.localPlayerController = fieldAt(localPlayer, localPlayerLayout, 'm_PlayerController').readPointer().toString();
                sample.isLocalPawn = sample.controllerPawn === sample.address &&
                                     sample.localPlayerController === pawnController.toString();
              }
            }
            if (!localController.isNull()) {
              sample.localPawn = fieldAt(localController, controller, 'm_Pawn').readPointer().toString();
              sample.isLocalPawn = sample.localPawn === sample.address;
              sample.localPlayer = fieldAt(localController, playerController, 'm_LocalPlayer').readPointer().toString();
            }
            root.samples.push(sample);
          } catch (error) { root.samples.push({slot:i, error:String(error)}); }
        }
        root.stable = list.add(Number(versionField.offset)).readS32() === versionField.value &&
                      list.add(Number(sizeField.offset)).readS32() === sizeField.value &&
                      list.add(Number(itemField.offset)).readPointer().equals(items);
        if (!root.stable) root.samples = [];
      }
      return output;
    } finally {
      if (attached !== null) threadDetach(attached);
    }
  }
};
