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
    def __init__(self, library="libgcloudsync.so"):
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
        with (ROOT / "build/readonly_native" / library).open("rb") as stream:
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
