"""Execute built ARM64 code in Unicorn; mock Android services, not hook logic.

Run with tools/test-venv/Scripts/python.exe tests/test_native.py after a build.
This verifies CPU/ABI behavior, not kernel cache coherency or game compatibility.
"""
import math
from pathlib import Path
import random
import struct
import subprocess
import unittest

from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
import unicorn.arm64_const as r

ROOT = Path(__file__).resolve().parents[1]
BASE, STACK, DATA, STUBS = 0x10000000, 0x20000000, 0x30000000, 0x40000000
TARGET, VENEER, STOP = 0x50000000, 0x50010000, STUBS + 0xF000
X = [getattr(r, f"UC_ARM64_REG_X{i}") for i in range(31)]
Q = [getattr(r, f"UC_ARM64_REG_Q{i}") for i in range(32)]
U64 = (1 << 64) - 1


class Machine:
    def __init__(self, library="libhook.so"):
        self.uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        self.symbols, self.callbacks, self.imports = {}, {}, {}
        self.services = {
            "__android_log_print": lambda: 0,
            "vsnprintf": lambda: 0,
            "strerror": lambda: DATA + 0xF000,
            "__errno": lambda: DATA + 0xF100,
            "mprotect": lambda: 0,
            "munmap": lambda: 0,
            "pthread_detach": lambda: 0,
            "pthread_create": lambda: 0,
            "acosf": self.acosf,
            "memcpy": self.memcpy,
        }
        with (ROOT / "build/native" / library).open("rb") as stream:
            elf = ELFFile(stream)
            segments = [s for s in elf.iter_segments() if s["p_type"] == "PT_LOAD"]
            limit = max(s["p_vaddr"] + s["p_memsz"] for s in segments)
            self.uc.mem_map(BASE, (limit + 4095) & ~4095)
            for seg in segments:
                self.uc.mem_write(BASE + seg["p_vaddr"], seg.data())
            for sym in elf.get_section_by_name(".symtab").iter_symbols():
                if sym["st_shndx"] != "SHN_UNDEF":
                    self.symbols[sym.name] = BASE + sym["st_value"]
            for section in elf.iter_sections():
                if section["sh_type"] != "SHT_RELA":
                    continue
                dynsym = elf.get_section(section["sh_link"])
                for rel in section.iter_relocations():
                    typ, addend = rel["r_info_type"], rel["r_addend"]
                    if typ == 1027:  # R_AARCH64_RELATIVE
                        value = BASE + addend
                    elif typ in (1025, 1026):
                        sym = dynsym.get_symbol(rel["r_info_sym"])
                        if sym["st_shndx"] != "SHN_UNDEF":
                            value = BASE + sym["st_value"] + addend
                        else:
                            value = STUBS + len(self.imports) * 16
                            self.imports[value] = sym.name
                    else:
                        raise AssertionError(f"Unhandled ELF relocation {typ}")
                    self.write64(BASE + rel["r_offset"], value)
        for addr in (STACK, DATA, STUBS, TARGET, VENEER):
            self.uc.mem_map(addr, 0x10000)
        self.uc.mem_write(STUBS, struct.pack("<I", 0xD65F03C0) * (0x10000 // 4))
        self.uc.mem_write(DATA + 0xF000, b"mock error\0")
        self.uc.reg_write(r.UC_ARM64_REG_TPIDR_EL0, DATA + 0xF800)
        self.uc.reg_write(r.UC_ARM64_REG_CPACR_EL1, 3 << 20)
        self.uc.reg_write(r.UC_ARM64_REG_SP, STACK + 0xF000)
        self.uc.hook_add(UC_HOOK_CODE, self.dispatch)

    def addr(self, name):
        return self.symbols[name]

    def reg(self, i):
        return self.uc.reg_read(X[i])

    def write32(self, address, value):
        self.uc.mem_write(address, struct.pack("<I", value & 0xFFFFFFFF))

    def write64(self, address, value):
        self.uc.mem_write(address, struct.pack("<Q", value & U64))

    def read32(self, address):
        return struct.unpack("<I", self.uc.mem_read(address, 4))[0]

    def read64(self, address):
        return struct.unpack("<Q", self.uc.mem_read(address, 8))[0]

    def float_reg(self, index=0):
        bits = self.uc.reg_read(Q[index]) & 0xFFFFFFFF
        return struct.unpack("<f", struct.pack("<I", bits))[0]

    def set_float_reg(self, value, index=0):
        bits = struct.unpack("<I", struct.pack("<f", value))[0]
        self.uc.reg_write(Q[index], bits)

    def acosf(self):
        self.set_float_reg(math.acos(self.float_reg()))
        return 0

    def memcpy(self):
        self.uc.mem_write(self.reg(0), bytes(self.uc.mem_read(self.reg(1), self.reg(2))))
        return self.reg(0)

    def dispatch(self, uc, address, size, _):
        if address == STOP:
            uc.emu_stop()
        elif address in self.callbacks:
            self.callbacks[address]()
        elif address in self.imports:
            name = self.imports[address]
            if name not in self.services:
                raise AssertionError(f"Unexpected Android service: {name}")
            result = self.services[name]()
            uc.reg_write(X[0], result & U64)
            uc.reg_write(r.UC_ARM64_REG_PC, self.reg(30))

    def call(self, name, *args):
        self.uc.reg_write(r.UC_ARM64_REG_SP, STACK + 0xF000)
        self.uc.reg_write(X[30], STOP)
        for i, value in enumerate(args):
            self.uc.reg_write(X[i], value & U64)
        self.uc.emu_start(self.addr(name), STOP, count=100000)
        if self.uc.reg_read(r.UC_ARM64_REG_PC) != STOP:
            raise AssertionError("Function did not return within instruction budget")
        return self.reg(0)


class NativeTests(unittest.TestCase):
    def test_prepare_veneer_on_4k_and_16k_pages(self):
        for page_size in (4096, 16384):
            for fallback in (False, True):
                with self.subTest(page_size=page_size, fallback=fallback):
                    m = Machine("libpatch_test.so")
                    m.write32(TARGET + 8, 0x14000004)
                    reads, maps, protections, unmapped = [], [], [], []

                    def fopen():
                        reads.clear()
                        return 1

                    def fgets():
                        if reads: return 0
                        reads.append(1)
                        line = f"{TARGET:x}-{TARGET + 0x10000:x} r-xp 00000000 00:00 0\n\0"
                        m.uc.mem_write(m.reg(0), line.encode())
                        return m.reg(0)

                    def sscanf():
                        m.write64(m.reg(2), TARGET)
                        m.write64(m.reg(3), TARGET + 0x10000)
                        fmt = bytes(m.uc.mem_read(m.reg(1), 32)).split(b"\0")[0]
                        if b"%4s" in fmt:
                            m.uc.mem_write(m.reg(4), b"r-xp\0")
                            return 3
                        return 2

                    def mmap():
                        self.assertEqual(m.reg(1), page_size)
                        self.assertFalse(m.reg(3) & 0x10, "must never use MAP_FIXED")
                        maps.append(m.reg(0))
                        return STUBS if fallback and len(maps) == 1 else VENEER

                    m.services.update({"sysconf": lambda: page_size, "fopen": fopen,
                        "fgets": fgets, "sscanf": sscanf, "fclose": lambda: 0, "mmap": mmap,
                        "mprotect": lambda: protections.append(m.reg(2)) or 0,
                        "munmap": lambda: unmapped.append(m.reg(0)) or 0})
                    self.assertEqual(m.call("cf_patch_prepare", DATA, TARGET + 8, 0x14000004,
                                            BASE + 0x100, 0, DATA + 512, 256), 1)
                    self.assertEqual(m.read64(DATA + 24), TARGET)
                    self.assertEqual(m.read64(DATA + 32), page_size)
                    self.assertEqual(m.read32(VENEER), 0x58000051)
                    self.assertEqual(m.read32(VENEER + 4), 0xD61F0220)
                    self.assertEqual(m.read64(VENEER + 8), BASE + 0x100)
                    self.assertEqual(protections, [5])
                    self.assertEqual(unmapped, [STUBS] if fallback else [])
                    self.assertEqual(m.read32(TARGET + 8), 0x14000004)
                    self.assertEqual(m.call("cf_patch_commit", DATA, 1, DATA + 512, 256), 1)
                    self.assertEqual(m.call("cf_branch_decode", TARGET + 8,
                                            m.read32(TARGET + 8), 0, DATA + 800), 1)
                    self.assertEqual(m.read64(DATA + 800), VENEER)

    def test_branch_boundaries_and_rejection(self):
        m = Machine("libpatch_test.so")
        for link in (0, 1):
            for delta in (-0x8000000, -4, 0, 4, 0x7FFFFFC):
                with self.subTest(link=link, delta=delta):
                    self.assertEqual(m.call("cf_branch_encode", TARGET, TARGET + delta, link, DATA), 1)
                    word = m.read32(DATA)
                    self.assertEqual(word >> 26, 0x25 if link else 0x05)
                    self.assertEqual(m.call("cf_branch_decode", TARGET, word, link, DATA + 8), 1)
                    self.assertEqual(m.read64(DATA + 8), TARGET + delta)
            for delta in (-0x8000004, 0x8000000, 1, 2):
                self.assertEqual(m.call("cf_branch_encode", TARGET, TARGET + delta, link, DATA), 0)
        self.assertEqual(m.call("cf_branch_decode", TARGET, 0xD65F03C0, 0, DATA), 0)
        self.assertEqual(m.call("cf_branch_decode", 0, 0x17FFFFFF, 0, DATA), 0)

    def test_patch_batch_and_failure_paths(self):
        for scenario in ("success", "same_page", "wrong_bytes", "permission_failure", "race"):
            with self.subTest(scenario=scenario):
                m = Machine("libpatch_test.so")
                addresses = [TARGET + 4, TARGET + (8 if scenario == "same_page" else 0x1004)]
                original, replacement = [0x14000010, 0x94000020], [0x14004000, 0x94003C00]
                for i, addr in enumerate(addresses):
                    m.write32(addr, original[i])
                    m.uc.mem_write(DATA + i * 48, struct.pack(
                        "<QIIQQQi?3x", addr, original[i], replacement[i],
                        VENEER + i * 4096, addr & ~4095, 4096, 5, False))
                calls, unmapped = [], []

                def protect():
                    calls.append((m.reg(0), m.reg(1), m.reg(2)))
                    if len(calls) == 2:
                        if scenario == "permission_failure": return -1
                        if scenario == "race": m.write32(addresses[1], 0xD503201F)
                    return 0

                m.services["mprotect"] = protect
                m.services["munmap"] = lambda: unmapped.append(m.reg(0)) or 0
                if scenario == "wrong_bytes": m.write32(addresses[1], 0xD503201F)
                result = m.call("cf_patch_commit", DATA, 2, DATA + 512, 256)
                if scenario in ("success", "same_page"):
                    self.assertEqual(result, 1)
                    self.assertEqual([m.read32(a) for a in addresses], replacement)
                    self.assertEqual([c[2] for c in calls], [7, 7, 5, 5])
                else:
                    self.assertEqual(result, 0)
                    self.assertEqual(m.read32(addresses[0]), original[0])
                    if scenario == "wrong_bytes": self.assertEqual(calls, [])
                    if scenario == "permission_failure":
                        self.assertEqual(m.read32(addresses[1]), original[1])
                        self.assertEqual([c[2] for c in calls], [7, 7, 5])
                    if scenario == "race": self.assertEqual(m.read32(addresses[1]), 0xD503201F)
                self.assertEqual(m.read32(TARGET), 0)
                m.call("cf_patch_discard_unpublished", DATA, 2)
                if scenario in ("success", "same_page"): self.assertEqual(unmapped, [])
                if scenario == "race": self.assertEqual(unmapped, [VENEER + 4096])

    def test_bridge_preserves_machine_state_and_returns_to_callsite(self):
        m = Machine()
        rng = random.Random(301)
        xs, qs = [rng.getrandbits(64) for _ in X], [rng.getrandbits(128) for _ in Q]
        sp, original_rotate = STACK + 0xF000, STUBS + 0xE000
        m.write64(m.addr("cf_original_rotate"), original_rotate)
        # The actual callsite and absolute near veneer used by the patch engine.
        m.write32(TARGET, 0x94000000 | ((VENEER - TARGET) // 4))
        m.uc.mem_write(VENEER, struct.pack("<IIQ", 0x58000051, 0xD61F0220, m.addr("cf_rotate_bridge")))
        for reg, value in zip(X, xs): m.uc.reg_write(reg, value)
        for reg, value in zip(Q, qs): m.uc.reg_write(reg, value)
        m.uc.reg_write(r.UC_ARM64_REG_SP, sp)
        m.uc.reg_write(r.UC_ARM64_REG_NZCV, 0xA0000000)
        m.uc.reg_write(r.UC_ARM64_REG_FPCR, 0x400000)
        m.uc.reg_write(r.UC_ARM64_REG_FPSR, 0x10)
        m.uc.mem_write(sp, struct.pack("<fI", 0.1, 0x12345678))
        visited = []

        def callback():
            frame, return_pc = m.reg(0), m.reg(30)
            self.assertEqual(frame, sp - 800)
            self.assertEqual(m.read64(frame + 248), sp)
            for i in range(32):
                self.assertEqual(int.from_bytes(m.uc.mem_read(frame + 256 + 16*i, 16), "little"), qs[i])
            m.uc.mem_write(sp, struct.pack("<f", 0.4))
            for reg in X: m.uc.reg_write(reg, 0xDEADBEEF)
            for reg in Q: m.uc.reg_write(reg, 0xBAD)
            for reg in (r.UC_ARM64_REG_NZCV, r.UC_ARM64_REG_FPCR, r.UC_ARM64_REG_FPSR):
                m.uc.reg_write(reg, 0)
            m.uc.reg_write(r.UC_ARM64_REG_PC, return_pc)
            visited.append("callback")

        def original():
            self.assertEqual(m.uc.reg_read(r.UC_ARM64_REG_SP), sp)
            for i in range(31):
                expected = original_rotate if i == 17 else TARGET + 4 if i == 30 else xs[i]
                self.assertEqual(m.reg(i), expected, f"x{i}")
            for i, reg in enumerate(Q): self.assertEqual(m.uc.reg_read(reg), qs[i], f"q{i}")
            self.assertEqual(m.uc.reg_read(r.UC_ARM64_REG_NZCV), 0xA0000000)
            self.assertEqual(m.uc.reg_read(r.UC_ARM64_REG_FPCR), 0x400000)
            self.assertEqual(m.uc.reg_read(r.UC_ARM64_REG_FPSR), 0x10)
            self.assertEqual(m.read32(sp + 4), 0x12345678)
            visited.append("original")
            m.uc.reg_write(r.UC_ARM64_REG_PC, m.reg(30))

        m.callbacks[m.addr("cf_adaptive_apply")] = callback
        m.callbacks[original_rotate] = original
        m.uc.emu_start(TARGET, TARGET + 4, count=10000)
        self.assertEqual(visited, ["callback", "original"])
        self.assertEqual(m.uc.reg_read(r.UC_ARM64_REG_PC), TARGET + 4)

    def test_adaptive_behavior_and_invalid_inputs(self):
        for delta, dt, angle, expected in ((0.1, 0.016, 10, 0.4), (0.4, 0.016, 10, 0.4),
                                          (0.1, 0.016, 0, 0.1), (0.1, 0, 10, 0.1)):
            with self.subTest(delta=delta, dt=dt, angle=angle):
                m = Machine()
                dt_func, slot = STUBS + 0xE000, DATA + 0x1000
                m.write64(m.addr("g_get_delta_time"), dt_func)
                m.write64(DATA + 248, slot)
                m.uc.mem_write(slot, struct.pack("<f", delta))
                components = [0, 0, 0, 1, 0, math.sin(math.radians(angle / 2)), 0,
                              math.cos(math.radians(angle / 2))]
                for i, value in enumerate(components):
                    m.uc.mem_write(DATA + 256 + i*16, struct.pack("<f", value))

                def get_dt():
                    m.set_float_reg(dt)
                    m.uc.reg_write(r.UC_ARM64_REG_PC, m.reg(30))

                m.callbacks[dt_func] = get_dt
                m.call("cf_adaptive_apply", DATA)
                result = struct.unpack("<f", m.uc.mem_read(slot, 4))[0]
                self.assertAlmostEqual(result, expected, places=5)

    def test_aim_tail_patch_returns_hfa(self):
        m = Machine()
        m.uc.mem_write(TARGET, struct.pack("<IIII", 0xAA1F03E0, 0xAA1F03E1,
                                          0x14000000 | ((VENEER - TARGET - 8)//4), 0x320003E0))
        m.uc.mem_write(VENEER, struct.pack("<IIQ", 0x58000051, 0xD61F0220, m.addr("hooked_aim_offset")))
        m.uc.reg_write(X[30], STOP)
        m.uc.emu_start(TARGET, STOP, count=1000)
        self.assertEqual([m.float_reg(i) for i in range(3)], [0.0, 0.125, 0.0])
        self.assertEqual(m.read32(TARGET + 12), 0x320003E0)

    def test_start_once_and_retry_after_thread_creation_failure(self):
        m = Machine()
        results, calls = [11, 0], []

        def create():
            calls.append(m.reg(2))
            return results.pop(0)

        m.services["pthread_create"] = create
        self.assertEqual(m.call("cf_payload_start"), 11)
        self.assertEqual(m.call("cf_payload_start"), 0)
        self.assertEqual(m.call("cf_payload_start"), 0)
        self.assertEqual(len(calls), 2)


if __name__ == "__main__":
    # Same production patch source, exported for boundary/failure-path tests.
    clang = ROOT / "tools/downloads/android-ndk-r27c/toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe"
    subprocess.run([str(clang), "--target=aarch64-linux-android23", "-shared", "-fPIC", "-O2",
                    str(ROOT / "native/memory_patch.c"), "-o",
                    str(ROOT / "build/native/libpatch_test.so")], check=True)
    unittest.main(verbosity=2)
