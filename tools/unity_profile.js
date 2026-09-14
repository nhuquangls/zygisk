'use strict';

// Native offsets below are supported only for the binary whose accessors were
// inspected in this session. Metadata offsets remain resolved by name.
let verifiedUnityBase = null;
function verifyUnityProfile() {
  const module = Process.getModuleByName('libunity.so'), base = module.base;
  if (verifiedUnityBase !== null && base.equals(verifiedUnityBase)) return;
  const offset = base.add(32).readU64().toNumber();
  const stride = base.add(54).readU16(), count = base.add(56).readU16();
  if (stride !== 56 || count < 1 || count > 64 || offset < 64 || offset > 65536)
    throw new Error('Unsupported Unity ELF headers');
  let buildId = null;
  for (let i = 0; i < count; ++i) {
    const ph = base.add(offset + i * stride);
    if (ph.readU32() !== 4) continue;
    const note = base.add(ph.add(16).readU64()), length = ph.add(40).readU64().toNumber();
    if (length > 65536) throw new Error('Unsupported Unity notes');
    for (let cursor = 0; cursor + 12 <= length;) {
      const p = note.add(cursor), nameSize = p.readU32(), descSize = p.add(4).readU32();
      if (nameSize > length || descSize > length) break;
      const desc = cursor + 12 + Math.ceil(nameSize / 4) * 4;
      if (desc + descSize > length) break;
      if (p.add(8).readU32() === 3 && nameSize === 4 && descSize <= 64 && p.add(12).readUtf8String(3) === 'GNU') {
        buildId = Array.from(new Uint8Array(note.add(desc).readByteArray(descSize)), x => x.toString(16).padStart(2, '0')).join('');
        break;
      }
      cursor = desc + Math.ceil(descSize / 4) * 4;
    }
  }
  if (buildId !== '1a60ff52f7bb4ad5de0465b12a83aba3d7af0700')
    throw new Error('Unity native layout has not been verified for build ' + buildId);
  verifiedUnityBase = base;
}
