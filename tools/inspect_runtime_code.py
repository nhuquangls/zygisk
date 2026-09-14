"""Read/disassemble selected live IL2CPP wrappers and resolve Unity icall names.

Does not invoke the inspected getter or install hooks.
"""
import argparse
import json
from pathlib import Path
import frida

SCRIPT = r'''
rpc.exports = {
  inspect(methods, icalls) {
    const mod = Process.getModuleByName('libil2cpp.so');
    const resolve = new NativeFunction(mod.getExportByName('il2cpp_resolve_icall'), 'pointer', ['pointer']);
    const result = [];
    function disassemble(name, address) {
      const record = {name, address:address.toString(), instructions:[]};
      const owner = Process.findModuleByAddress(address);
      if (owner) { record.module = owner.name; record.rva = address.sub(owner.base).toString(); }
      if (!address.isNull()) {
        try {
          for (let i = 0; i < 80; ++i) {
            const instruction = Instruction.parse(address.add(i * 4));
            record.instructions.push(instruction.address.toString() + ': ' + instruction.toString());
            if (instruction.mnemonic === 'ret' || instruction.mnemonic === 'br') break;
          }
        } catch (error) {record.error = String(error);}
      }
      result.push(record);
    }
    for (const method of methods) disassemble(method.name, mod.base.add(ptr(method.rva)));
    for (const name of icalls) disassemble(name, resolve(Memory.allocUtf8String(name)));
    return result;
  }
};
'''

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--pid', required=True, type=int)
    parser.add_argument('--metadata', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    metadata = json.loads(args.metadata.read_text(encoding='utf-8'))
    wanted = {'INTERNAL_get_position', 'get_position', 'INTERNAL_get_worldToCameraMatrix',
              'INTERNAL_get_projectionMatrix', 'INTERNAL_get_localToWorldMatrix'}
    methods = [{**m, 'name':c['name'] + '.' + m['name']} for c in metadata['classes']
               if c['namespace'] == 'UnityEngine' and c['name'] in ['Transform','Camera']
               for m in c['methods'] if m['name'] in wanted]
    names = ['UnityEngine.Transform::INTERNAL_get_position',
             'UnityEngine.Camera::INTERNAL_get_worldToCameraMatrix',
             'UnityEngine.Camera::INTERNAL_get_projectionMatrix']
    device = frida.get_device_manager().add_remote_device('127.0.0.1:27043')
    session = device.attach(args.pid)
    try:
        script = session.create_script(SCRIPT)
        script.load()
        result = script.exports_sync.inspect(methods, names)
    finally:
        session.detach()
    args.output.write_text(json.dumps(result, indent=2), encoding='utf-8')
    for record in result:
        print(record['name'], record.get('module'), record['address'])
        for instruction in record['instructions']: print(instruction)

if __name__ == '__main__':
    main()
