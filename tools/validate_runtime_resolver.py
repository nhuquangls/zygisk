"""Compare the built native resolver with runtime metadata in an existing process.

The exact local library must already be copied to --device-library. The helper
loads it for this synchronous check, does not start the payload worker, and
releases its dlopen reference afterward. No hooks or game methods are used.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import subprocess
import re
import frida

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = r'''
rpc.exports = {
  validate(path, offsets, count) {
    const dlopen = new NativeFunction(Module.getGlobalExportByName('dlopen'), 'pointer', ['pointer', 'int']);
    const dlclose = new NativeFunction(Module.getGlobalExportByName('dlclose'), 'int', ['pointer']);
    const dlsym = new NativeFunction(Module.getGlobalExportByName('dlsym'), 'pointer', ['pointer', 'pointer']);
    const dlerror = new NativeFunction(Module.getGlobalExportByName('dlerror'), 'pointer', []);
    const handle = dlopen(Memory.allocUtf8String(path), 2);
    if (handle.isNull()) throw new Error(dlerror().readUtf8String());
    try {
      const start = dlsym(handle, Memory.allocUtf8String('cf_payload_start'));
      if (start.isNull()) throw new Error('Payload entry missing');
      const base = start.sub(offsets.cf_payload_start);
      const find = new NativeFunction(base.add(offsets.cf_find_module), 'bool', ['pointer', 'pointer']);
      const resolve = new NativeFunction(base.add(offsets.cf_metadata_resolve), 'bool', ['pointer', 'pointer', 'pointer', 'uint64']);
      const module = Memory.alloc(160), layout = Memory.alloc(count * 8 + 16), error = Memory.alloc(256);
      const found = find(Memory.allocUtf8String('libil2cpp.so'), module);
      const result = {pid:Process.id, found, payloadBase:base.toString()};
      if (found) {
        result.il2cppBase = module.readPointer().toString();
        result.buildId = module.add(24).readUtf8String();
        result.resolved = resolve(module, layout, error, 256);
        result.error = error.readUtf8String();
        result.offsets = [];
        for (let i = 0; i < count; ++i) result.offsets.push(layout.add(i * 8).readU64().toString());
        result.manager = layout.add(count * 8).readPointer().toString();
        result.targetList = layout.add(count * 8 + 8).readPointer().toString();
      }
      return result;
    } finally {
      if (dlclose(handle) !== 0) throw new Error('Unable to release validation library');
    }
  }
};
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--pid', required=True, type=int)
    parser.add_argument('--remote', default='127.0.0.1:27043')
    parser.add_argument('--device-library', required=True)
    parser.add_argument('--library', type=Path, default=ROOT / 'build/readonly_native/libgcloudsync.so')
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--metadata', type=Path, help='Compare every native offset with a runtime_metadata.py capture')
    args = parser.parse_args()
    nm = ROOT / 'tools/downloads/android-ndk-r27c/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-nm.exe'
    symbols = subprocess.check_output([str(nm), '--defined-only', str(args.library)], text=True)
    required = {'cf_payload_start', 'cf_find_module', 'cf_metadata_resolve'}
    offsets = {}
    for line in symbols.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] in required:
            offsets[parts[2]] = int(parts[0], 16)
    if set(offsets) != required:
        raise RuntimeError('Use the matching unstripped build with all resolver symbols')
    # The count is obtained from this build's enum, not from game field offsets.
    header = (ROOT / 'native/il2cpp_metadata.h').read_text(encoding='utf-8')
    field_count = header.split('typedef enum CfFieldId {', 1)[1].split('CF_METADATA_FIELD_COUNT', 1)[0].count(',')
    device = frida.get_device_manager().add_remote_device(args.remote)
    session = device.attach(args.pid)
    try:
        script = session.create_script(SCRIPT)
        script.load()
        result = script.exports_sync.validate(args.device_library, offsets, field_count)
    finally:
        session.detach()
    result['librarySha256'] = hashlib.sha256(args.library.read_bytes()).hexdigest()
    result['deviceLibrary'] = args.device_library
    result['validatedAt'] = datetime.now(timezone.utc).isoformat()
    if args.metadata and result.get('resolved'):
        metadata = json.loads(args.metadata.read_text(encoding='utf-8'))
        source = (ROOT / 'native/il2cpp_metadata.c').read_text(encoding='utf-8')
        specs = re.findall(r'\{"([^"]+)", "([^"]+)", "([^"]+)", \d+, \d+\}', source)
        if len(specs) != field_count:
            raise RuntimeError('Field specifications and enum disagree')
        compared = []
        for index, (namespace, name, field_name) in enumerate(specs):
            klass = next(c for c in metadata['classes'] if c['namespace'] == namespace and c['name'] == name)
            field = next(f for f in klass['fields'] if f['name'] == field_name)
            if int(result['offsets'][index]) != int(field['offset']):
                raise RuntimeError('Native/metadata mismatch: ' + name + '.' + field_name)
            compared.append(namespace + '.' + name + '.' + field_name)
        result['matchedFields'] = compared
        result['metadataCapture'] = str(args.metadata)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(json.dumps(result, indent=2))
    if not result.get('resolved'):
        raise SystemExit(1)


if __name__ == '__main__':
    main()
