"""Run production ARM64 code in Unicorn; Android kernel/JNI is not emulated.

Build first, then: tools/test-venv/Scripts/python.exe tests/test_readonly.py
"""
import io
import math
import re
import struct
import subprocess
import unittest
import zipfile
from elftools.elf.elffile import ELFFile
from arm64_machine import Machine, ROOT, DATA, TARGET, U64

MATRIX, POINT, VIEW, RESULT = DATA, DATA + 128, DATA + 160, DATA + 192
CANDIDATES, SETTINGS = DATA + 256, DATA + 8192
ERRNO = DATA + 0xF100
IDENTITY = [1., 0., 0., 0., 0., 1., 0., 0., 0., 0., 1., 0., 0., 0., 0., 1.]


def dex_uleb(data, offset):
    value = 0
    for shift in range(0, 35, 7):
        byte = data[offset]
        offset += 1
        value |= (byte & 0x7f) << shift
        if byte < 0x80:
            return value, offset
    raise AssertionError('Invalid DEX ULEB128 value')


def dex_native_methods(path, descriptor):
    """Return {(name, signature)} for native methods on one DEX class."""
    data = path.read_bytes()
    if data[:4] != b'dex\n':
        raise AssertionError('Invalid DEX header')

    def u16(offset): return struct.unpack_from('<H', data, offset)[0]
    def u32(offset): return struct.unpack_from('<I', data, offset)[0]

    string_count, string_off = u32(56), u32(60)
    strings = []
    for index in range(string_count):
        item = u32(string_off + index * 4)
        _, item = dex_uleb(data, item)  # UTF-16 length; strings here contain no NUL.
        end = data.index(0, item)
        strings.append(data[item:end].decode('utf-8'))

    type_count, type_off = u32(64), u32(68)
    types = [strings[u32(type_off + index * 4)] for index in range(type_count)]

    proto_count, proto_off = u32(72), u32(76)
    protos = []
    for index in range(proto_count):
        item = proto_off + index * 12
        return_type, params_off = u32(item + 4), u32(item + 8)
        params = [] if not params_off else [types[u16(params_off + 4 + i * 2)]
                                             for i in range(u32(params_off))]
        protos.append('(' + ''.join(params) + ')' + types[return_type])

    method_count, method_off = u32(88), u32(92)
    methods = [(u16(method_off + i * 8), u16(method_off + i * 8 + 2),
                u32(method_off + i * 8 + 4)) for i in range(method_count)]

    class_count, class_off = u32(96), u32(100)
    for index in range(class_count):
        item = class_off + index * 32
        if types[u32(item)] != descriptor:
            continue
        cursor = u32(item + 24)
        if not cursor:
            return set()
        counts = []
        for _ in range(4):
            count, cursor = dex_uleb(data, cursor)
            counts.append(count)
        for _ in range(counts[0] + counts[1]):
            _, cursor = dex_uleb(data, cursor)  # field_idx_diff
            _, cursor = dex_uleb(data, cursor)  # access_flags
        result = set()
        for count in counts[2:]:
            method_index = 0
            for _ in range(count):
                diff, cursor = dex_uleb(data, cursor)
                access, cursor = dex_uleb(data, cursor)
                _, cursor = dex_uleb(data, cursor)  # code_off
                method_index += diff
                if access & 0x100:  # ACC_NATIVE
                    class_index, proto_index, name_index = methods[method_index]
                    if types[class_index] == descriptor:
                        result.add((strings[name_index], protos[proto_index]))
        return result
    raise AssertionError(f'DEX class not found: {descriptor}')


class BridgeContractTests(unittest.TestCase):
    def test_embedded_dex_matches_every_registered_native(self):
        dex = (ROOT / 'build/bridge/dex/classes.dex').read_bytes()
        header = (ROOT / 'build/bridge/bridge_dex.h').read_text(encoding='ascii')
        array = header[header.index('{') + 1:header.rindex('}')]
        self.assertEqual(bytes(int(value) for value in re.findall(r'\d+', array)), dex)

        source = (ROOT / 'native/android_input.c').read_text(encoding='utf-8')
        registered = set(re.findall(
            r'\{\s*"([^"]+)"\s*,\s*"([^"]+)"\s*,\s*\(void \*\)\s*\w+\s*\}',
            source))
        self.assertEqual(dex_native_methods(ROOT / 'build/bridge/dex/classes.dex',
                                            'Lrt/internal/Bridge;'), registered)
        self.assertIn('sizeof(methods) / sizeof(methods[0])', source)


def machine(library='libreadmath_test.so'):
    m = Machine(library)
    def memset():
        m.uc.mem_write(m.reg(0), bytes([m.reg(1) & 255]) * m.reg(2))
        return m.reg(0)
    def cstring(address):
        result = bytearray()
        while address and len(result) < 1024:
            byte = m.uc.mem_read(address + len(result), 1)[0]
            if byte == 0: return bytes(result)
            result.append(byte)
        raise AssertionError('Bad string in native code')
    def compare(a, b):
        return (a > b) - (a < b)
    def strrchr():
        index = cstring(m.reg(0)).rfind(bytes([m.reg(1) & 255]))
        return 0 if index < 0 else m.reg(0) + index
    def readv():
        assert m.reg(0) == 123 and m.reg(2) == 1 and m.reg(4) == 1 and m.reg(5) == 0
        local, size = struct.unpack('<QQ', m.uc.mem_read(m.reg(1), 16))
        remote, remote_size = struct.unpack('<QQ', m.uc.mem_read(m.reg(3), 16))
        assert size == remote_size
        m.uc.mem_write(local, bytes(m.uc.mem_read(remote, size)))
        return size
    m.services.update({
        'memset': memset,
        'getpid': lambda: 123,
        'process_vm_readv': readv,
        'sleep': lambda: 0,
        'strrchr': strrchr,
        'strnlen': lambda: min(len(cstring(m.reg(0))), m.reg(1)),
        'strcmp': lambda: compare(cstring(m.reg(0)), cstring(m.reg(1))),
        'memcmp': lambda: compare(bytes(m.uc.mem_read(m.reg(0), m.reg(2))),
                                  bytes(m.uc.mem_read(m.reg(1), m.reg(2)))),
    })
    return m


class ReadOnlyTests(unittest.TestCase):
    def test_read_preserves_source(self):
        m = machine()
        value = bytes(range(256)) * 16
        m.uc.mem_write(TARGET, value)
        self.assertEqual(m.call('cf_read_self', TARGET, DATA, len(value)), 1)
        self.assertEqual(bytes(m.uc.mem_read(DATA, len(value))), value)
        self.assertEqual(bytes(m.uc.mem_read(TARGET, len(value))), value)

    def test_partial_or_denied_read_clears_destination(self):
        for transferred, error in [(3, 0), (0, 0), (-1, 1), (-1, 14)]:
            with self.subTest(transferred=transferred, error=error):
                m = machine()
                m.uc.mem_write(DATA, b'old data')
                def fail():
                    m.write32(ERRNO, error)
                    return transferred
                m.services['process_vm_readv'] = fail
                self.assertEqual(m.call('cf_read_self', TARGET, DATA, 8), 0)
                self.assertEqual(bytes(m.uc.mem_read(DATA, 8)), b'\0' * 8)
                self.assertEqual(m.read32(ERRNO), error if transferred < 0 else 5)

    def test_interrupted_read_retries(self):
        m = machine()
        original = m.services['process_vm_readv']
        calls = []
        def interrupt_once():
            calls.append(1)
            if len(calls) == 1:
                m.write32(ERRNO, 4)
                return -1
            return original()
        m.services['process_vm_readv'] = interrupt_once
        m.write64(TARGET, 0x12345678)
        self.assertEqual(m.call('cf_read_self', TARGET, DATA, 8), 1)
        self.assertEqual(m.read64(DATA), 0x12345678)
        self.assertEqual(len(calls), 2)

    def test_bad_addresses_and_lengths_do_not_call_kernel(self):
        for source, dest, size in [(0, DATA, 8), (U64 - 3, DATA, 8),
                                   (TARGET, DATA, 4097), (TARGET, DATA, 0),
                                   (TARGET, 0, 8)]:
            m = machine()
            m.services.pop('process_vm_readv')
            self.assertEqual(m.call('cf_read_self', source, dest, size), 0)

    def test_pointer_chain_and_null_hop(self):
        m = machine()
        m.write64(TARGET + 16, TARGET + 128)
        m.write64(TARGET + 128 + 24, TARGET + 1024)
        m.uc.mem_write(DATA, struct.pack('<QQ', 16, 24))
        self.assertEqual(m.call('cf_follow_chain', TARGET, DATA, 2, RESULT), 1)
        self.assertEqual(m.read64(RESULT), TARGET + 1024)
        m.write64(TARGET + 128 + 24, 0)
        self.assertEqual(m.call('cf_follow_chain', TARGET, DATA, 2, RESULT), 0)
        self.assertEqual(m.read64(RESULT), 0)

    def test_pointer_chain_rejects_overflow_and_unaligned_slot(self):
        for base, offset, count in [(U64 - 7, 16, 1), (TARGET, 1, 1),
                                    (TARGET, 0, 0), (TARGET, 0, 9)]:
            m = machine()
            m.services.pop('process_vm_readv')
            m.write64(DATA, offset)
            self.assertEqual(m.call('cf_follow_chain', base, DATA, count, RESULT), 0)
            self.assertEqual(m.read64(RESULT), 0)


class MathTests(unittest.TestCase):
    def setUp(self):
        self.m = machine()
        self.m.uc.mem_write(MATRIX, struct.pack('<16f', *IDENTITY))
        self.m.uc.mem_write(VIEW, struct.pack('<2f', 1000., 800.))
        self.m.uc.mem_write(SETTINGS, struct.pack('<ffQ', 200., .05, 100))

    def project(self, point):
        self.m.uc.mem_write(POINT, struct.pack('<3f', *point))
        valid = self.m.call('cf_world_to_screen', MATRIX, POINT, VIEW, RESULT)
        screen = struct.unpack('<2f', self.m.uc.mem_read(RESULT, 8))
        return valid, screen

    def candidate(self, index, ident, point, flags=(True, True, True, False)):
        self.m.uc.mem_write(CANDIDATES + 24*index,
                             struct.pack('<Q3f4?', ident, *point, *flags))

    def select(self, count, sample=1000, now=1000):
        valid = self.m.call('cf_select_aim', MATRIX, VIEW, CANDIDATES, count,
                            SETTINGS, sample, now, RESULT)
        return valid, struct.unpack('<Q4f', self.m.uc.mem_read(RESULT, 24))

    def test_projection_axes_and_perspective(self):
        self.assertEqual(self.project((0., 0., 0.)), (1, (500., 400.)))
        self.assertEqual(self.project((1., 1., 0.)), (1, (1000., 0.)))
        matrix = IDENTITY.copy()
        matrix[3], matrix[7], matrix[14], matrix[15] = .2, -.4, 1., 0.
        self.m.uc.mem_write(MATRIX, struct.pack('<16f', *matrix))
        valid, screen = self.project((0., 0., 2.))
        self.assertEqual(valid, 1)
        self.assertAlmostEqual(screen[0], 550., places=4)
        self.assertAlmostEqual(screen[1], 480., places=4)
        self.assertEqual(self.project((0., 0., -1.)), (0, (0., 0.)))

    def test_projection_rejects_invalid_values_and_offscreen(self):
        for point in [(math.nan, 0., 0.), (0., math.inf, 0.), (2., 0., 0.)]:
            self.assertEqual(self.project(point), (0, (0., 0.)))
        for index, value in [(0, math.nan), (15, 0.), (15, -1.)]:
            matrix = IDENTITY.copy()
            matrix[index] = value
            self.m.uc.mem_write(MATRIX, struct.pack('<16f', *matrix))
            self.assertEqual(self.project((0., 0., 0.)), (0, (0., 0.)))

    def test_selection_nearest_and_smooth_without_mutating_snapshot(self):
        self.candidate(0, 10, (.2, .2, 0.))
        self.candidate(1, 20, (.1, 0., 0.))
        before = bytes(self.m.uc.mem_read(CANDIDATES, 48))
        valid, result = self.select(2)
        self.assertEqual(valid, 1)
        self.assertEqual(result[0], 20)
        self.assertAlmostEqual(result[3], 2.5, places=4)
        self.assertEqual(result[4], 0.)
        self.assertEqual(bytes(self.m.uc.mem_read(CANDIDATES, 48)), before)

    def test_filters_do_not_select_friend_dead_hidden_or_invulnerable(self):
        for flags in [(False, True, True, False), (True, False, True, False),
                      (True, True, False, False), (True, True, True, True)]:
            self.candidate(0, 1, (0., 0., 0.), flags)
            self.assertEqual(self.select(1), (0, (0, 0., 0., 0., 0.)))

    def test_fov_stale_future_and_empty_frames_clear_result(self):
        self.candidate(0, 1, (.5, 0., 0.))
        self.assertEqual(self.select(1)[0], 0)
        self.candidate(0, 1, (.1, 0., 0.))
        self.assertEqual(self.select(1, now=1100)[0], 1)
        for count, sample, now in [(1, 1000, 1101), (1, 1001, 1000),
                                    (0, 1000, 1000), (257, 1000, 1000)]:
            self.assertEqual(self.select(count, sample, now), (0, (0, 0., 0., 0., 0.)))

    def test_invalid_smoothing_is_rejected(self):
        self.candidate(0, 1, (.1, 0., 0.))
        for smooth in [0., -1., 1.01, math.nan]:
            self.m.uc.mem_write(SETTINGS, struct.pack('<ffQ', 200., smooth, 100))
            self.assertEqual(self.select(1)[0], 0)


class MetadataTests(unittest.TestCase):
    def test_scene_waits_for_runtime_thread_registration(self):
        from arm64_machine import X
        import unicorn.arm64_const as r
        for ready in (None, 0, 1, 2):
            with self.subTest(ready=ready):
                m = machine()
                resolved = []
                def ret(value):
                    m.uc.reg_write(X[0], value)
                    m.uc.reg_write(r.UC_ARM64_REG_PC, m.reg(30))
                def module():
                    m.uc.mem_write(m.reg(1), struct.pack('<QQQ129s7x', TARGET,
                        TARGET, TARGET+0xc000000, b'1a60ff52f7bb4ad5de0465b12a83aba3d7af0700'))
                    ret(1)
                def read_ready():
                    self.assertEqual(m.reg(0), TARGET+0xbe000e0)
                    self.assertEqual(m.reg(2), 4)
                    if ready is not None: m.write32(m.reg(1), ready)
                    ret(int(ready is not None))
                def resolve():
                    resolved.append(True)
                    ret(0)
                m.uc.mem_write(DATA, struct.pack('<QQQ129s7x', TARGET, TARGET,
                    TARGET+0xc000000, b'a8793b51fee671e98de0cc0ad42bb85ffd5d0677'))
                m.callbacks[m.addr('cf_find_module')] = module
                m.callbacks[m.addr('cf_read_self')] = read_ready
                m.callbacks[m.addr('cf_metadata_resolve')] = resolve
                m.services.update(calloc=lambda: DATA+0x3000, free=lambda: 0,
                                  snprintf=lambda: 0)
                self.assertEqual(m.call('cf_scene_open', DATA, DATA+0x2000, 256), 0)
                self.assertEqual(bool(resolved), ready == 1)

    def test_does_not_attach_before_game_assembly_is_published(self):
        from arm64_machine import STUBS, X
        import unicorn.arm64_const as r
        for phase in ('empty', 'other_assembly', 'game_ready'):
            with self.subTest(phase=phase):
                m = machine()
                calls = []
                def ret(value):
                    m.uc.reg_write(X[0], value)
                    m.uc.reg_write(r.UC_ARM64_REG_PC, m.reg(30))
                table = [STUBS + 0xD000 + i * 16 for i in range(15)]
                def api_open():
                    m.uc.mem_write(m.reg(1), struct.pack('<15Q', *table))
                    ret(1)
                def assemblies():
                    calls.append('assemblies')
                    m.write64(m.reg(1), 0 if phase == 'empty' else 1)
                    ret(DATA + 0xA000)
                def attach():
                    calls.append('attach')
                    ret(TARGET + 0x300)
                def detach():
                    calls.append('detach')
                    ret(0)
                m.write64(DATA + 0xA000, TARGET + 0x100)
                name = b'Assembly-CSharp.dll' if phase == 'game_ready' else b'mscorlib.dll'
                m.uc.mem_write(DATA + 0xA100, name + b'\0')
                m.callbacks[m.addr('cf_exports_open')] = lambda: ret(1)
                m.callbacks[m.addr('cf_metadata_api_open')] = api_open
                m.callbacks[table[0]] = lambda: ret(TARGET)  # non-null static domain
                m.callbacks[table[1]] = assemblies
                m.callbacks[table[2]] = lambda: ret(TARGET + 0x200)
                m.callbacks[table[3]] = lambda: ret(DATA + 0xA100)
                m.callbacks[table[4]] = lambda: ret(0)  # stop at missing class
                m.callbacks[table[12]] = lambda: ret(0)
                m.callbacks[table[13]] = attach
                m.callbacks[table[14]] = detach
                m.services['snprintf'] = lambda: 0
                self.assertEqual(m.call('cf_metadata_resolve', DATA, DATA+0x1000,
                                        DATA+0x2000, 256), 0)
                expected = ['assemblies', 'attach', 'detach'] if phase == 'game_ready' else ['assemblies']
                self.assertEqual(calls, expected)

    def test_field_boundaries_type_and_static_storage(self):
        m = machine()
        for offset, width, size, flags, actual, expected, valid in [
                (16, 8, 24, 1, 18, 18, 1), (20, 4, 24, 1, 12, 12, 1),
                (15, 8, 24, 1, 18, 18, 0), (24, 8, 24, 1, 18, 18, 0),
                (20, 8, 24, 1, 18, 18, 0), (16, 0, 24, 1, 18, 18, 0),
                (U64 - 7, 8, 24, 1, 18, 18, 0), (16, U64, 24, 1, 18, 18, 0),
                (16, 8, 24, 0x11, 18, 18, 0), (16, 8, 24, 0x41, 18, 18, 0),
                (16, 8, 24, 1, 17, 18, 0)]:
            with self.subTest(offset=offset, width=width, flags=flags, kind=actual):
                self.assertEqual(m.call('cf_metadata_instance_field_valid', offset, width,
                                       size, flags, actual, expected), valid)


class AimPolicyTests(unittest.TestCase):
    def fixture(self, dx, dy=0, ident=42):
        m = machine()
        snap = DATA + 0x2000
        m.uc.mem_write(snap, bytes(6272))
        m.uc.mem_write(snap, struct.pack('<16f', *IDENTITY))
        m.uc.mem_write(snap+64, struct.pack('<Q3f4?', ident, dx/500, -dy/400, 0, True, True, True, False))
        m.write64(snap+6208, 1)
        m.write64(snap+6216, 1000000000)
        m.uc.mem_write(snap+6228, struct.pack('<f', 5.671282))
        m.write64(snap+6248, 99)
        m.uc.mem_write(snap+6258, b'\1\1')
        m.write64(snap+6264, ident)
        m.uc.mem_write(VIEW, struct.pack('<2f', 1000, 800))
        return m, snap

    def choose(self, m, snap, locked=0, now=1000000000, projection=5.671282):
        m.uc.mem_write(snap+6228, struct.pack('<f', projection))
        m.set_float_reg(projection)
        return m.call('cf_aim_from_snapshot', snap, VIEW, locked, now, RESULT)

    def test_uses_only_game_target_without_custom_acquisition_radius(self):
        m, snap = self.fixture(450)
        m.uc.mem_write(snap+88, struct.pack('<Q3f4?', 43, .02, 0, 0, True, True, True, False))
        m.write64(snap+6208, 2)
        self.assertEqual(self.choose(m, snap), 1)
        self.assertEqual(m.read64(RESULT), 42)
        self.assertAlmostEqual(struct.unpack('<f', m.uc.mem_read(RESULT+16, 4))[0], 450, places=3)

    def test_rejects_missing_game_target_stale_inactive_wrong_lock_or_scope(self):
        for kind in ('missing', 'stale', 'future', 'doing', 'lock', 'zoom', 'sniper'):
            m, snap = self.fixture(100)
            now, zoom, locked = 1000000000, 5.671282, 42
            if kind == 'missing': m.write64(snap+6264, 43)
            if kind == 'stale': now += 100000001
            if kind == 'future': now -= 1
            if kind == 'doing':
                m.uc.mem_write(snap+6259, b'\0')
                locked = 0
            if kind == 'lock': locked = 43
            if kind == 'zoom': zoom = 4.0
            if kind == 'sniper': m.uc.mem_write(snap+6258, b'\0')
            self.assertEqual(self.choose(m, snap, locked, now, zoom), 0, kind)
            self.assertEqual(bytes(m.uc.mem_read(RESULT, 24)), bytes(24))

    def test_game_trigger_requires_sniper_flag_and_full_scope_projection(self):
        for sniper, projection, expected in [
                (0, 1.25, 0), (0, 5.671282, 0),
                (1, 1.25, 0), (1, 5.671282, 1)]:
            m, snap = self.fixture(100)
            m.uc.mem_write(snap+6258, bytes([sniper]))
            self.assertEqual(self.choose(m, snap, locked=0,
                                         projection=projection), expected)

    def test_builtin_aim_flag_does_not_block_active_game_target(self):
        m, snap = self.fixture(100)
        m.uc.mem_write(snap+6256, b'\1')
        self.assertEqual(self.choose(m, snap, 42), 1)
        self.assertEqual(m.read64(RESULT), 42)

    def test_game_doing_state_is_authoritative_over_visible_field(self):
        m, snap = self.fixture(100)
        m.uc.mem_write(snap+86, b'\0')  # candidate.visible
        self.assertEqual(self.choose(m, snap), 1)
        self.assertEqual(m.read64(RESULT), 42)

    def test_locked_target_is_rejected_after_game_trigger_ends(self):
        m, snap = self.fixture(100)
        m.write64(snap+6264, 0)
        m.uc.mem_write(snap+6259, b'\0')
        self.assertEqual(self.choose(m, snap, 42), 0)
        self.assertEqual(bytes(m.uc.mem_read(RESULT, 24)), bytes(24))

    def test_world_velocity_prediction_filters_and_resets_on_target_change(self):
        m = machine()
        predictor, head, out = DATA+0x5000, DATA+0x5100, DATA+0x5200
        m.uc.mem_write(predictor, bytes(48))
        m.uc.mem_write(head, struct.pack('<3f', 0, 0, 0))
        self.assertEqual(m.call('cf_target_predict', predictor, 42, head, 1000000000, out), 1)
        self.assertEqual(struct.unpack('<3f', m.uc.mem_read(out, 12)), (0, 0, 0))
        m.uc.mem_write(head, struct.pack('<3f', .1, 0, 0))
        self.assertEqual(m.call('cf_target_predict', predictor, 42, head, 1020000000, out), 1)
        self.assertAlmostEqual(struct.unpack('<f', m.uc.mem_read(out, 4))[0], .55, places=4)
        m.uc.mem_write(head, struct.pack('<3f', 2, 3, 4))
        self.assertEqual(m.call('cf_target_predict', predictor, 43, head, 1040000000, out), 1)
        self.assertEqual(struct.unpack('<3f', m.uc.mem_read(out, 12)), (2, 3, 4))


class GyroControllerTests(unittest.TestCase):
    STATE = DATA + 0x6000
    COMMAND = DATA + 0x6100

    def fixture(self, dx=100, dy=50, projection=5.671282):
        m = machine()
        snap = DATA + 0x2000
        m.uc.mem_write(snap, bytes(6272))
        m.uc.mem_write(snap, struct.pack('<16f', *IDENTITY))
        m.uc.mem_write(snap+64, struct.pack(
            '<Q3f4?', 42, dx/500, -dy/400, 0, True, True, True, False))
        m.write64(snap+6208, 1)
        m.write64(snap+6216, 1000000000)
        m.uc.mem_write(snap+6228, struct.pack('<f', projection))
        m.write64(snap+6248, 99)
        m.uc.mem_write(snap+6258, b'\1\1')  # sniper enabled + doing
        m.write64(snap+6264, 42)
        m.uc.mem_write(VIEW, struct.pack('<2f', 1000, 800))
        m.uc.mem_write(self.STATE, bytes(256))
        m.uc.mem_write(self.COMMAND, bytes(64))
        m.call('cf_gyro_controller_reset', self.STATE)
        return m, snap

    def step(self, m, snap, now=1000000000):
        return m.call('cf_gyro_controller_step', self.STATE, snap, VIEW,
                      now, self.COMMAND)

    def command(self, m):
        target = m.read64(self.COMMAND)
        ex, ey, sx, sy = struct.unpack('<4f', m.uc.mem_read(self.COMMAND+8, 16))
        mode = m.read32(self.COMMAND+24)
        active = bool(m.uc.mem_read(self.COMMAND+28, 1)[0])
        settled = bool(m.uc.mem_read(self.COMMAND+29, 1)[0])
        return target, ex, ey, sx, sy, mode, active, settled

    def test_target_right_and_below_maps_to_negative_x_positive_y(self):
        m, snap = self.fixture()
        self.assertEqual(self.step(m, snap), 1)
        target, ex, ey, sx, sy, mode, active, settled = self.command(m)
        self.assertEqual((target, mode, active, settled), (42, 2, True, False))
        self.assertAlmostEqual(ex, 100, places=3)
        self.assertAlmostEqual(ey, 50, places=3)
        self.assertLess(sx, 0)
        self.assertGreater(sy, 0)
        self.assertLessEqual(abs(sx), .15)
        self.assertLessEqual(abs(sy), .15)

    def test_trigger_is_idle_until_sniper_scope_is_open(self):
        m, snap = self.fixture(projection=4.0)
        self.assertEqual(self.step(m, snap), 0)
        self.assertEqual(self.command(m), (0, 0, 0, 0, 0, 0, False, False))

    def test_tracking_gets_exactly_500ms_after_game_trigger_drops(self):
        m, snap = self.fixture()
        self.assertEqual(self.step(m, snap), 1)
        m.uc.mem_write(snap+6259, b'\0')
        m.write64(snap+6264, 0)
        m.write64(snap+6216, 1016000000)
        self.assertEqual(self.step(m, snap, 1016000000), 1)
        self.assertEqual(self.command(m)[5:7], (2, True))
        m.write64(snap+6216, 1515999999)
        self.assertEqual(self.step(m, snap, 1515999999), 1)
        self.assertEqual(self.command(m)[5:7], (2, True))
        m.write64(snap+6216, 1516000000)
        self.assertEqual(self.step(m, snap, 1516000000), 0)
        self.assertEqual(self.command(m), (0, 0, 0, 0, 0, 0, False, False))

    def test_settled_cycle_rearms_on_next_game_trigger(self):
        m, snap = self.fixture(dx=2, dy=1)
        self.assertEqual(self.step(m, snap), 1)
        target, _, _, sx, sy, mode, active, settled = self.command(m)
        self.assertEqual((target, mode, active, settled), (42, 3, False, True))
        self.assertEqual((sx, sy), (0, 0))

        m.uc.mem_write(snap+72, struct.pack('<3f', .2, -.125, 0))
        m.write64(snap+6216, 1016000000)
        self.assertEqual(self.step(m, snap, 1016000000), 1)
        self.assertEqual(self.command(m)[5:], (3, False, True))

        m.uc.mem_write(snap+6259, b'\0')
        m.write64(snap+6264, 0)
        m.write64(snap+6216, 1032000000)
        self.assertEqual(self.step(m, snap, 1032000000), 1)
        self.assertEqual(self.command(m)[5:], (3, False, True))

        m.uc.mem_write(snap+6259, b'\1')
        m.write64(snap+6264, 42)
        m.write64(snap+6216, 1048000000)
        self.assertEqual(self.step(m, snap, 1048000000), 1)
        self.assertEqual(self.command(m)[5:7], (2, True))

    def test_controller_does_not_gate_on_visible_field(self):
        m, snap = self.fixture()
        m.uc.mem_write(snap+86, b'\0')  # candidate.visible
        self.assertEqual(self.step(m, snap), 1)
        self.assertEqual(self.command(m)[5:7], (2, True))

    def test_invalid_target_gets_no_correction_during_trigger_grace(self):
        m, snap = self.fixture()
        self.assertEqual(self.step(m, snap), 1)
        m.uc.mem_write(snap+6259, b'\0')
        m.write64(snap+6264, 0)
        m.uc.mem_write(snap+85, b'\0')  # candidate.alive
        m.write64(snap+6216, 1016000000)
        self.assertEqual(self.step(m, snap, 1016000000), 0)
        target, _, _, sx, sy, mode, active, _ = self.command(m)
        self.assertEqual((target, sx, sy, mode, active), (42, 0, 0, 2, False))

    def test_trigger_requires_sniper_flag_and_scope(self):
        for sniper, projection in ((0, 1.25), (0, 5.671282),
                                    (1, 1.25), (1, 5.671282)):
            m, snap = self.fixture(projection=projection)
            m.uc.mem_write(snap+6258, bytes([sniper]))
            expected = int(sniper == 1 and projection >= 5.30)
            self.assertEqual(self.step(m, snap), expected)
            target, _, _, sx, sy, mode, active, settled = self.command(m)
            if expected:
                self.assertEqual((target, mode, active, settled),
                                 (42, 2, True, False))
                self.assertNotEqual((sx, sy), (0, 0))
            else:
                self.assertEqual((target, mode, active, settled),
                                 (0, 0, False, False))
                self.assertEqual((sx, sy), (0, 0))

    def test_sniper_policy_still_requires_doing_and_current_target(self):
        for missing in ('doing', 'target'):
            m, snap = self.fixture()
            if missing == 'doing':
                m.uc.mem_write(snap+6259, b'\0')
            else:
                m.write64(snap+6264, 0)
            self.assertEqual(self.step(m, snap), 0, missing)
            self.assertEqual(self.command(m)[5], 0, missing)

    def test_active_cycle_has_two_second_fail_safe(self):
        m, snap = self.fixture(dx=100)
        self.assertEqual(self.step(m, snap), 1)
        m.write64(snap+6216, 3000000001)
        self.assertEqual(self.step(m, snap, 3000000001), 1)
        self.assertEqual(self.command(m)[5:], (3, False, True))


class SceneModePolicyTests(unittest.TestCase):
    def test_team_modes_use_camp_but_individual_mode_uses_identity(self):
        m = machine()
        enemy = 'cf_scene_candidate_is_enemy'
        self.assertEqual(m.call(enemy, 0, 1, 1, 42, 99), 0)
        self.assertEqual(m.call(enemy, 0, 2, 1, 42, 99), 1)
        self.assertEqual(m.call(enemy, 1, 1, 1, 42, 99), 1)
        self.assertEqual(m.call(enemy, 1, 1, 1, 99, 99), 0)

    def test_idle_gate_requests_full_targets_only_for_sniper_session(self):
        m = machine()
        snap = DATA + 0x2000
        m.uc.mem_write(snap, bytes(6272))
        m.uc.mem_write(snap+6228, struct.pack('<f', 5.671282))
        m.uc.mem_write(snap+6258, b'\1\1')
        m.write64(snap+6264, 42)
        self.assertEqual(m.call('cf_scene_sniper_scope_active', snap), 1)
        self.assertEqual(m.call('cf_scene_needs_targets', snap, 0, 1, 0), 1)
        self.assertEqual(m.call('cf_scene_needs_targets', snap, 0, 0, 42), 0)
        self.assertEqual(m.call('cf_scene_needs_targets', snap, 0, 0, 41), 1)
        self.assertEqual(m.call('cf_scene_needs_targets', snap, 1, 0, 42), 1)
        m.uc.mem_write(snap+6259, b'\0')
        self.assertEqual(m.call('cf_scene_needs_targets', snap, 1, 0, 42), 1)
        m.uc.mem_write(snap+6259, b'\1')
        m.uc.mem_write(snap+6228, struct.pack('<f', 4.0))
        self.assertEqual(m.call('cf_scene_needs_targets', snap, 1, 0, 42), 0)
        m.uc.mem_write(snap+6228, struct.pack('<f', 5.671282))
        m.uc.mem_write(snap+6258, b'\0')
        self.assertEqual(m.call('cf_scene_needs_targets', snap, 1, 0, 42), 0)


class TransformTests(unittest.TestCase):
    def fixture(self):
        m = machine()
        native, data, trs, parents = TARGET+0x100, TARGET+0x200, TARGET+0x1000, TARGET+0x2000
        m.write64(TARGET+16, native)
        m.write64(native+0x40, data)
        m.write32(native+0x48, 0)
        m.write64(data+8, trs)
        m.write64(data+16, parents)
        m.uc.mem_write(trs, struct.pack('<10f', 1, 0, 0, 0, 0, 0, 1, 1, 1, 1))
        m.uc.mem_write(trs+40, struct.pack('<10f', 10, 20, 30, 0, 0, math.sqrt(.5), math.sqrt(.5), 2, 2, 2))
        m.write32(parents, 1)
        m.write32(parents+4, -1)
        return m, trs, parents

    def test_parent_translation_rotation_and_scale(self):
        m, trs, parents = self.fixture()
        before = bytes(m.uc.mem_read(trs, 80))
        self.assertEqual(m.call('cf_transform_position', TARGET, 16, RESULT), 1)
        for actual, expected in zip(struct.unpack('<3f', m.uc.mem_read(RESULT, 12)), (10, 22, 30)):
            self.assertAlmostEqual(actual, expected, places=4)
        self.assertEqual(bytes(m.uc.mem_read(trs, 80)), before)

    def test_mid_chest_blends_root_and_upper_body(self):
        m = machine()
        root, upper = DATA+0x5000, DATA+0x5100
        m.uc.mem_write(root, struct.pack('<3f', 2, 3, 4))
        m.uc.mem_write(upper, struct.pack('<3f', 2, 4, 4))
        self.assertEqual(m.call('cf_chest_point', root, upper, RESULT), 1)
        actual = struct.unpack('<3f', m.uc.mem_read(RESULT, 12))
        for value, expected in zip(actual, (2, 3.78, 4)):
            self.assertAlmostEqual(value, expected, places=5)
        m.uc.mem_write(upper, struct.pack('<3f', 2, 3.1, 4))
        self.assertEqual(m.call('cf_chest_point', root, upper, RESULT), 0)
        self.assertEqual(bytes(m.uc.mem_read(RESULT, 12)), bytes(12))

    def test_invalid_parent_cycle_rotation_and_pointer_fail_closed(self):
        for kind in ('cycle', 'index', 'rotation', 'null', 'nan'):
            m, trs, parents = self.fixture()
            if kind == 'cycle': m.write32(parents+4, 0)
            if kind == 'index': m.write32(parents, 65536)
            if kind == 'rotation': m.uc.mem_write(trs+40+24, struct.pack('<f', 2))
            if kind == 'null': m.write64(TARGET+16, 0)
            if kind == 'nan': m.uc.mem_write(trs, struct.pack('<f', math.nan))
            self.assertEqual(m.call('cf_transform_position', TARGET, 16, RESULT), 0, kind)
            self.assertEqual(bytes(m.uc.mem_read(RESULT, 12)), bytes(12))


class CompanionTests(unittest.TestCase):
    PANEL = b'NVTCapacitiveTouchScreen\0'
    DOWN = [(0, 0, 3, 0x2f, 9), (0, 0, 3, 0x39, 0x5A17), (0, 0, 3, 0x35, 12345),
            (0, 0, 3, 0x36, 5432), (0, 0, 3, 0x30, 12), (0, 0, 3, 0x3a, 1),
            (0, 0, 1, 0x14a, 1), (0, 0, 0, 0, 0)]
    MOVE = [(0, 0, 3, 0x2f, 9), (0, 0, 3, 0x35, 12350), (0, 0, 3, 0x36, 5440), (0, 0, 0, 0, 0)]
    UP = [(0, 0, 3, 0x2f, 9), (0, 0, 3, 0x39, -1),
          (0, 0, 1, 0x14a, 0), (0, 0, 0, 0, 0)]

    def run_stream(self, commands, panel=True):
        m = machine()
        packet = b''.join(struct.pack('<IBBHii', *c) for c in commands)
        chunks = [packet[:5], packet[5:]]
        replies, writes, opened = [], [], []

        def read():
            if not chunks: return 0
            chunk, want = chunks.pop(0), m.reg(2)
            if len(chunk) > want:  # never write past the caller's buffer
                m.uc.mem_write(m.reg(1), chunk[:want])
                chunks.insert(0, chunk[want:])
                return want
            m.uc.mem_write(m.reg(1), chunk)
            return len(chunk)

        def open_node():
            opened.append(1)
            return 7 if panel else -1

        def ioctl():
            if panel: m.uc.mem_write(m.reg(2), self.PANEL)  # buffer is the third argument
            return 0 if panel else -1

        def write_events():
            buf = bytes(m.uc.mem_read(m.reg(1), m.reg(2)))
            writes.append([struct.unpack('<QQHHi', buf[i:i + 24]) for i in range(0, len(buf), 24)])
            return m.reg(2)

        def send_reply():
            replies.append(struct.unpack('<i', bytes(m.uc.mem_read(m.reg(1), 4)))[0])
            return m.reg(2)

        def strncmp():
            n = m.reg(2)
            a = bytes(m.uc.mem_read(m.reg(0), n))
            b = bytes(m.uc.mem_read(m.reg(1), n))
            return (a > b) - (a < b)

        m.services.update(fcntl=lambda: 0, open=open_node, ioctl=ioctl, write=write_events,
                          send=send_reply, read=read, close=lambda: 0,
                          snprintf=lambda: 0, strncmp=strncmp)
        m.call('cf_input_companion', 6)
        return writes, replies, opened

    def test_down_move_up_stream_protocol_b_events(self):
        writes, replies, _ = self.run_stream([
            (0x43464934, 1, 9, 0x5A17, 12345, 5432),
            (0x43464934, 2, 9, 0x5A17, 12350, 5440),
            (0x43464934, 3, 9, 0x5A17, 0, 0)])
        self.assertEqual(replies, [0, 0, 0])
        self.assertEqual(writes, [self.DOWN, self.MOVE, self.UP])

    def test_contact_asserts_btn_touch_instead_of_hover(self):
        writes, _, _ = self.run_stream([
            (0x43464934, 1, 9, 0x5A17, 12345, 5432),
            (0x43464934, 3, 9, 0x5A17, 0, 0)])
        keys = [[event for event in batch if event[2] == 1] for batch in writes]
        self.assertEqual(keys, [[(0, 0, 1, 0x14a, 1)], [(0, 0, 1, 0x14a, 0)]])

    def test_invalid_command_drops_channel_and_releases_contact(self):
        writes, replies, _ = self.run_stream([
            (0x43464934, 1, 9, 0x5A17, 12345, 5432),
            (0, 2, 9, 0x5A17, 3, 4)])
        self.assertEqual(replies, [0])
        self.assertEqual(writes, [self.DOWN, self.UP])

    def test_disconnect_releases_held_contact(self):
        # The command list ends after one DOWN: the socket EOF path must
        # release the still-pressed virtual contact.
        writes, replies, _ = self.run_stream([(0x43464934, 1, 9, 0x5A17, 12345, 5432)])
        self.assertEqual(replies, [0])
        self.assertEqual(writes, [self.DOWN, self.UP])

    def test_missing_panel_scans_nodes_and_reports_error(self):
        writes, replies, opened = self.run_stream([(0x43464934, 1, 9, 0x5A17, 1, 1)],
                                                  panel=False)
        self.assertEqual(len(opened), 64)
        self.assertEqual(replies, [19])  # ENODEV
        self.assertEqual(writes, [])


class TouchPackTests(unittest.TestCase):
    def pack(self, action, rotation, layout, x, y):
        m = machine()
        out = DATA + 0x2000
        result = m.call('cf_touch_pack', action, rotation, layout, x, y, out)
        fields = struct.unpack('<IBBHii', bytes(m.uc.mem_read(out, 16)))
        m.uc.mem_write(DATA + 0x2100, struct.pack('<IBBHii', *fields))
        return result, fields, m.call('cf_touch_valid', DATA + 0x2100)

    def test_anchor_round_trip_for_every_layout_and_rotation(self):
        for rotation, layout in [(1, 0), (1, 1), (3, 0), (3, 1)]:
            result, fields, valid = self.pack(1, rotation, layout, 1943, 736)
            self.assertEqual(result, 1)
            self.assertEqual(valid, 1)
            self.assertEqual(fields[:4], (0x43464934, 1, 9, 0x5A17))
            self.assertTrue(0 <= fields[4] <= 18400)
            self.assertTrue(0 <= fields[5] <= 29440)
        _, fields, _ = self.pack(1, 1, 0, 1943, 736)
        self.assertEqual(fields[4], (1840 - 1 - 736) * 18400 // (1840 - 1))
        self.assertEqual(fields[5], 1943 * 29440 // (2944 - 1))

    def test_bounds_and_action_rules(self):
        self.assertEqual(self.pack(2, 1, 0, 1943 + 420, 736 + 420)[0], 1)
        self.assertEqual(self.pack(2, 1, 0, 1943 + 421, 736)[0], 0)
        self.assertEqual(self.pack(2, 1, 0, 1943, 736 - 421)[0], 0)
        self.assertEqual(self.pack(1, 1, 0, 1944, 736)[0], 0)
        self.assertEqual(self.pack(3, 1, 0, 5, 5)[0], 1)
        self.assertEqual(self.pack(2, 0, 0, 1943, 736)[0], 0)
        self.assertEqual(self.pack(2, 2, 0, 1943, 736)[0], 0)
        self.assertEqual(self.pack(2, 1, 2, 1943, 736)[0], 0)
        self.assertEqual(self.pack(9, 1, 0, 1943, 736)[0], 0)
        self.assertEqual(self.pack(0, 1, 0, 1943, 736)[0], 0)


class ExportTests(unittest.TestCase):
    MODULE, EXPORTS, NAME = DATA, DATA + 512, DATA + 4096

    def fixture(self, gnu=False):
        m = machine()
        header = struct.pack('<16sHHIQQQIHHHHHH', b'\x7fELF\x02\x01\x01',
                             3, 183, 1, 0, 64, 0, 0, 64, 56, 2, 0, 0, 0)
        m.uc.mem_write(TARGET, header)
        m.uc.mem_write(TARGET + 64, struct.pack('<II6Q', 1, 5, 0, 0, 0, 4096, 4096, 4096))
        m.uc.mem_write(TARGET + 120, struct.pack('<II6Q', 2, 4, 0x200, 0x200, 0, 96, 96, 8))
        dynamic = [(6, 0x400), (5, 0x500), (10, 64), (11, 24),
                   (0x6ffffef5 if gnu else 4, 0x600), (0, 0)]
        m.uc.mem_write(TARGET + 0x200, b''.join(struct.pack('<qQ', *d) for d in dynamic))
        name = b'il2cpp_domain_get\0'
        m.uc.mem_write(TARGET + 0x500, b'\0' + name)
        m.uc.mem_write(TARGET + 0x418, struct.pack('<IBBHQQ', 1, 0x12, 0, 1, 0x800, 4))
        if gnu:
            hashed = 5381
            for c in name[:-1]: hashed = (hashed * 33 + c) & 0xffffffff
            m.uc.mem_write(TARGET + 0x600, struct.pack('<4IQ2I', 1, 1, 1, 5, U64, 1, hashed | 1))
        else:
            m.uc.mem_write(TARGET + 0x600, struct.pack('<5I', 1, 2, 1, 0, 0))
        m.uc.mem_write(self.MODULE, struct.pack('<QQQ129s7x', TARGET, TARGET, TARGET + 4096, b''))
        m.uc.mem_write(self.NAME, name)
        return m

    def test_resolves_exact_name_with_sysv_and_gnu_hash(self):
        for gnu in [False, True]:
            m = self.fixture(gnu)
            before = bytes(m.uc.mem_read(TARGET, 4096))
            self.assertEqual(m.call('cf_exports_open', self.MODULE, self.EXPORTS), 1)
            self.assertEqual(m.call('cf_exports_function', self.EXPORTS, self.NAME), TARGET + 0x800)
            m.uc.mem_write(self.NAME, b'il2cpp_domain_get_other\0')
            self.assertEqual(m.call('cf_exports_function', self.EXPORTS, self.NAME), 0)
            self.assertEqual(bytes(m.uc.mem_read(TARGET, 4096)), before)

    def test_refuses_nonexported_noncode_and_out_of_range_symbols(self):
        for info, visibility, section, value, name_offset in [
                (0x11, 0, 1, 0x800, 1), (0x12, 2, 1, 0x800, 1),
                (0x02, 0, 1, 0x800, 1), (0x12, 0, 0, 0x800, 1),
                (0x12, 0, 1, 0x1000, 1), (0x12, 0, 1, U64, 1),
                (0x12, 0, 1, 0x801, 1), (0x12, 0, 1, 0x800, 64)]:
            m = self.fixture()
            m.uc.mem_write(TARGET + 0x418, struct.pack('<IBBHQQ', name_offset, info,
                                                      visibility, section, value, 4))
            self.assertEqual(m.call('cf_exports_open', self.MODULE, self.EXPORTS), 1)
            self.assertEqual(m.call('cf_exports_function', self.EXPORTS, self.NAME), 0)
        m = self.fixture()
        m.write32(TARGET + 68, 4)  # readable load without execute permission
        self.assertEqual(m.call('cf_exports_open', self.MODULE, self.EXPORTS), 1)
        self.assertEqual(m.call('cf_exports_function', self.EXPORTS, self.NAME), 0)

    def test_bad_headers_or_unreadable_image_leave_empty_table(self):
        for address, value in [(TARGET, 0), (TARGET + 32, U64),
                               (TARGET + 0x208, 0x1000), (TARGET + 0x250, U64)]:
            m = self.fixture()
            m.write64(address, value)
            m.uc.mem_write(self.EXPORTS, b'\xff' * 1848)
            self.assertEqual(m.call('cf_exports_open', self.MODULE, self.EXPORTS), 0)
            self.assertEqual(bytes(m.uc.mem_read(self.EXPORTS, 1848)), b'\0' * 1848)
        m = self.fixture()
        m.services['process_vm_readv'] = lambda: -1
        self.assertEqual(m.call('cf_exports_open', self.MODULE, self.EXPORTS), 0)

    def test_bad_hash_sizes_indexes_and_cycles_are_bounded(self):
        for address, value in [(0x600, 0), (0x600, 0xffffffff),
                               (0x608, 2), (0x610, 1)]:
            m = self.fixture()
            m.write32(TARGET + address, value)
            self.assertEqual(m.call('cf_exports_open', self.MODULE, self.EXPORTS), 1)
            m.uc.mem_write(self.NAME, b'missing_symbol\0')
            self.assertEqual(m.call('cf_exports_function', self.EXPORTS, self.NAME), 0)
        for address, value in [(0x600, 0), (0x608, 0xffffffff), (0x618, 0), (0x618, 0xffffffff)]:
            m = self.fixture(gnu=True)
            m.write32(TARGET + address, value)
            self.assertEqual(m.call('cf_exports_open', self.MODULE, self.EXPORTS), 1)
            self.assertEqual(m.call('cf_exports_function', self.EXPORTS, self.NAME), 0)


class RuntimeTests(unittest.TestCase):
    def test_input_descriptor_checks_ipc_access_without_a_command(self):
        for send_error, received, read_error, expected in [
                (0, -1, 11, 0), (13, -1, 11, 13), (0, -1, 13, 13),
                (0, 0, 0, 104), (0, 1, 0, 71)]:
            with self.subTest(send_error=send_error, received=received, read_error=read_error):
                m = machine('libgcloudsync.so')
                m.services['fcntl'] = lambda: 0
                def send():
                    self.assertEqual(m.reg(0), 6)
                    self.assertEqual(m.reg(2), 0)
                    m.write32(ERRNO, send_error)
                    return -1 if send_error else 0
                def recv():
                    self.assertEqual(m.reg(0), 6)
                    self.assertEqual(m.reg(2), 1)
                    self.assertEqual(m.reg(3), 2 | 64)  # PEEK | DONTWAIT
                    m.write32(ERRNO, read_error)
                    return received
                m.services.update(send=send, recv=recv, sendto=send, recvfrom=recv)
                self.assertEqual(m.call('cf_payload_input_fd', 6), expected)
                if expected == 0: self.assertEqual(m.call('cf_payload_input_fd', 7), 22)

    def test_start_once_and_retry_after_thread_creation_failure(self):
        m = machine('libgcloudsync.so')
        results, calls = [11, 0], []
        def create():
            calls.append(m.reg(2))
            return results.pop(0)
        m.services['pthread_create'] = create
        self.assertEqual(m.call('cf_payload_start'), 11)
        self.assertEqual(m.call('cf_payload_status'), 0)
        self.assertEqual(m.call('cf_payload_start'), 0)
        self.assertEqual(m.call('cf_payload_start'), 0)
        self.assertEqual(m.call('cf_payload_status'), 1)
        self.assertEqual(len(calls), 2)

    def test_worker_failures_and_input_unavailable_without_vm(self):
        from arm64_machine import X
        import unicorn.arm64_const as r
        for found, readable, scene, expected in [
                (False, False, False, 2), (True, False, False, 3),
                (True, True, False, 5), (True, True, True, 8)]:
            m = machine('libgcloudsync.so')
            observed = []
            def ret(value):
                m.uc.reg_write(X[0], value)
                m.uc.reg_write(r.UC_ARM64_REG_PC, m.reg(30))
            def module():
                if found:
                    m.uc.mem_write(m.reg(1), struct.pack('<QQQ129s7x', TARGET, TARGET, TARGET+4096, b'test'))
                ret(int(found))
            def open_scene():
                m.uc.mem_write(m.reg(1), b'mock failure\0')
                ret(TARGET if scene else 0)
            def stop_after_tick():
                observed.append(m.read32(m.addr('g_status')))
                m.write32(m.addr('g_stop'), 1)
                return 0
            m.callbacks[m.addr('cf_find_module')] = module
            m.callbacks[m.addr('cf_scene_open')] = open_scene
            def poll_scene():
                m.uc.mem_write(m.reg(2), b'\1')
                ret(1)
            m.callbacks[m.addr('cf_scene_poll')] = poll_scene
            m.callbacks[m.addr('cf_scene_read_target')] = lambda: ret(1)
            for name in ['cf_input_snapshot', 'cf_input_enable', 'cf_input_shutdown', 'cf_scene_close']:
                m.callbacks[m.addr(name)] = lambda: ret(0)
            m.services['usleep'] = stop_after_tick
            if not readable: m.services['process_vm_readv'] = lambda: -1
            m.call('readonly_worker', 0)
            self.assertEqual(observed[-1] if observed else m.call('cf_payload_status'), expected)

    def test_idle_gate_skips_full_scene_read_until_sniper_trigger(self):
        from arm64_machine import X
        import unicorn.arm64_const as r
        for active, expected_full_reads in ((False, 0), (True, 1)):
            with self.subTest(active=active):
                m = machine('libgcloudsync.so')
                full_reads = []
                def ret(value):
                    m.uc.reg_write(X[0], value)
                    m.uc.reg_write(r.UC_ARM64_REG_PC, m.reg(30))
                def module():
                    m.uc.mem_write(m.reg(1), struct.pack(
                        '<QQQ129s7x', TARGET, TARGET, TARGET+4096, b'test'))
                    ret(1)
                def poll_scene():
                    snap = m.reg(1)
                    m.uc.mem_write(snap, bytes(6272))
                    m.write64(snap+6216, 1000000000)
                    m.uc.mem_write(snap+6228, struct.pack(
                        '<f', 5.671282 if active else 1.25))
                    m.write64(snap+6248, 99)
                    m.uc.mem_write(snap+6258,
                                   b'\1\1' if active else b'\1\0')
                    m.write64(snap+6264, 42 if active else 0)
                    m.uc.mem_write(m.reg(2), b'\0')
                    ret(1)
                def full_scene():
                    full_reads.append(1)
                    ret(0)
                def stop_after_tick():
                    m.write32(m.addr('g_stop'), 1)
                    return 0
                m.callbacks[m.addr('cf_find_module')] = module
                m.callbacks[m.addr('cf_scene_open')] = lambda: ret(TARGET)
                m.callbacks[m.addr('cf_scene_poll')] = poll_scene
                m.callbacks[m.addr('cf_scene_read_target')] = full_scene
                for name in ['cf_input_snapshot', 'cf_input_enable',
                             'cf_input_shutdown', 'cf_scene_close']:
                    m.callbacks[m.addr(name)] = lambda: ret(0)
                m.services['usleep'] = stop_after_tick
                m.call('readonly_worker', 0)
                self.assertEqual(len(full_reads), expected_full_reads)

    def test_binary_has_no_patch_engine_or_write_memory_imports(self):
        with (ROOT / 'build/readonly_native/libgcloudsync.so').open('rb') as stream:
            elf = ELFFile(stream)
            names = {s.name for s in elf.get_section_by_name('.symtab').iter_symbols()}
            imports = {s.name for s in elf.get_section_by_name('.dynsym').iter_symbols()
                       if s['st_shndx'] == 'SHN_UNDEF'}
        forbidden = {'mprotect', 'mmap', 'sigaction', 'process_vm_writev',
                     'DobbyHook', 'cf_patch_commit', 'shadow_init',
                     'maps_spoof_init', 'cf_libanort_patch_dispatchers',
                     'dlopen', 'dlsym'}
        self.assertFalse(names & forbidden)
        self.assertIn('process_vm_readv', imports)
        self.assertFalse(any(name.startswith('il2cpp_') for name in imports))

    def test_release_binaries_have_no_runtime_logging(self):
        paths = [ROOT / 'build/readonly_native/libgcloudsync.so',
                 ROOT / 'build/loader_libs/arm64-v8a/librt_shim.so']
        for path in paths:
            data = path.read_bytes()
            with path.open('rb') as stream:
                symbols = ELFFile(stream).get_section_by_name('.dynsym')
                imports = {s.name for s in symbols.iter_symbols()
                           if s['st_shndx'] == 'SHN_UNDEF'}
            self.assertNotIn('__android_log_print', imports, path.name)
            for tag in (b'SysLoader', b'SysRuntime', b'SysGyro'):
                self.assertNotIn(tag, data, path.name)

    def test_team_game_pvp_controller_profile_is_packaged(self):
        payload = (ROOT / 'build/readonly_native/libgcloudsync.so').read_bytes()
        self.assertIn(b'WNPVPGame.WNTeamGame\0', payload)
        self.assertIn(b'TeamGamePlayerController\0', payload)

    def test_individual_and_bomb_game_profiles_are_packaged(self):
        payload = (ROOT / 'build/readonly_native/libgcloudsync.so').read_bytes()
        for value in [b'WNIndivdualGame\0', b'IndivdualGame\0',
                      b'WNPVPGame.WNBombGame\0', b'BombGamePlayerPawn\0',
                      b'BombGamePlayerController\0']:
            self.assertIn(value, payload)

    def test_active_reader_uses_game_target_without_visible_or_list_scan(self):
        source = (ROOT / 'native/scene_snapshot.c').read_text(encoding='utf-8')
        direct = source[source.index('static bool scene_read_current_target'):
                        source.index('bool cf_scene_read(', source.index(
                            'static bool scene_read_current_target'))]
        self.assertIn('out->game_aim_target : retained_target', direct)
        self.assertIn('bool cf_scene_read_target(', source)
        self.assertNotIn('CF_TARGET_LIST', direct)
        self.assertNotIn('m_IsPawnVisible', source)
        payload = (ROOT / 'build/readonly_native/libgcloudsync.so').read_bytes()
        self.assertNotIn(b'm_IsPawnVisible\0', payload)

    def test_worker_retries_scene_binding_before_sampling(self):
        from arm64_machine import X
        import unicorn.arm64_const as r
        m = machine('libgcloudsync.so')
        attempts, waits = [], []
        def ret(value):
            m.uc.reg_write(X[0], value)
            m.uc.reg_write(r.UC_ARM64_REG_PC, m.reg(30))
        def module():
            m.uc.mem_write(m.reg(1), struct.pack('<QQQ129s7x', TARGET, TARGET, TARGET+4096, b''))
            ret(1)
        def scene():
            attempts.append(1)
            m.uc.mem_write(m.reg(1), b'not initialized\0')
            ret(TARGET if len(attempts) == 3 else 0)
        def snapshot():
            m.write32(m.addr('g_stop'), 1)
            ret(0)
        m.callbacks[m.addr('cf_find_module')] = module
        m.callbacks[m.addr('cf_scene_open')] = scene
        m.callbacks[m.addr('cf_scene_poll')] = snapshot
        for name in ['cf_input_snapshot', 'cf_input_enable', 'cf_input_shutdown', 'cf_scene_close']:
            m.callbacks[m.addr(name)] = lambda: ret(0)
        m.services['sleep'] = lambda: (waits.append(m.reg(0)) or 0)
        m.services['usleep'] = lambda: 0
        m.call('readonly_worker', 0)
        self.assertEqual(m.call('cf_payload_status'), 0)
        self.assertEqual(len(attempts), 3)
        self.assertEqual(waits, [1, 1])

    def test_packaged_payload_is_current_and_correctly_named(self):
        with zipfile.ZipFile(ROOT / 'output/rt_shim.zip') as archive:
            self.assertNotIn('payload/libhook.so', archive.namelist())
            payload = archive.read('payload/libgcloudsync.so')
            self.assertEqual(payload, (ROOT / 'build/readonly_module_stage/payload/libgcloudsync.so').read_bytes())
            self.assertIn(b'rt_shim', archive.read('module.prop'))
            rule = archive.read('sepolicy.rule').decode()
            self.assertIn('allow untrusted_app zygote unix_stream_socket', rule)
            symbols = ELFFile(io.BytesIO(payload)).get_section_by_name('.dynsym')
            exported = {s.name for s in symbols.iter_symbols() if s['st_shndx'] != 'SHN_UNDEF'}
            self.assertIn('cf_payload_start', exported)
            self.assertIn('cf_payload_status', exported)


if __name__ == '__main__':
    # The shipping probe's linker drops math/chain helpers until an adapter
    # references them. Test the same sources in a separate exported library.
    clang = ROOT / 'tools/downloads/android-ndk-r27c/toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe'
    subprocess.run([str(clang), '--target=aarch64-linux-android23', '-shared',
                    '-fPIC', '-O2', '-Wall', '-Wextra', '-Werror',
                    str(ROOT / 'native/readonly_memory.c'),
                    str(ROOT / 'native/readonly_module.c'),
                    str(ROOT / 'native/readonly_exports.c'),
                    str(ROOT / 'native/il2cpp_metadata.c'),
                    str(ROOT / 'native/aim_math.c'), str(ROOT / 'native/aim_policy.c'),
                    str(ROOT / 'native/gyro_controller.c'),
                    str(ROOT / 'native/scene_snapshot.c'),
                    str(ROOT / 'loader/jni/input_companion.c'), '-ldl', '-o',
                    str(ROOT / 'build/readonly_native/libreadmath_test.so')], check=True)
    unittest.main(verbosity=2)
