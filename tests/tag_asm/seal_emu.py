"""Run seal_core.S routines on an emulated Cortex-M0 (Unicorn) for testing."""
import subprocess, struct
from unicorn import *
from unicorn.arm_const import *

BASE, STACK_TOP, DATA = 0x1000, 0x20010000, 0x20000000
SENTINEL = 0x0F00


class Seal:
    def __init__(self, binpath="seal.bin", elfpath="seal.elf", count=True):
        nm = subprocess.run(["arm-none-eabi-nm", elfpath], capture_output=True, text=True).stdout
        self.sym = {l.split()[2]: int(l.split()[0], 16) for l in nm.splitlines() if len(l.split()) == 3}
        self.code = open(binpath, "rb").read()
        self.mu = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
        self.mu.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_M0)
        self.mu.mem_map(0x0, 0x10000)
        self.mu.mem_map(0x20000000, 0x10000)
        self.mu.mem_write(BASE, self.code)
        self.mu.mem_write(SENTINEL, b"\xfe\xe7" * 8)      # b . loop (never reached)
        self.icount = 0
        if count:
            self.mu.hook_add(UC_HOOK_CODE, self._hook)

    def _hook(self, uc, addr, size, ud):
        self.icount += 1

    def write(self, addr, data):
        self.mu.mem_write(addr, bytes(data))

    def read(self, addr, n):
        return bytes(self.mu.mem_read(addr, n))

    def call(self, name, *args, maxinsn=5_000_000):
        mu = self.mu
        for r in (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3,
                  UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7):
            mu.reg_write(r, 0xDEAD0000)
        for r, v in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3), args):
            mu.reg_write(r, v)
        mu.reg_write(UC_ARM_REG_SP, STACK_TOP)
        mu.reg_write(UC_ARM_REG_LR, SENTINEL | 1)
        # preserved-register check values
        saved = {r: mu.reg_read(r) for r in (UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7)}
        self.icount = 0
        mu.emu_start(self.sym[name] | 1, SENTINEL, count=maxinsn)
        for r, v in saved.items():
            assert mu.reg_read(r) == v, "callee-saved register clobbered"
        assert mu.reg_read(UC_ARM_REG_SP) == STACK_TOP, "stack imbalance"
        v = mu.reg_read(UC_ARM_REG_R0)
        return v

    def encode(self, data):
        self.write(DATA, data); self.write(DATA + 0x200, bytes(36))
        self.call("seal_encode", DATA, DATA + 0x200)
        return self.read(DATA + 0x200, 32)

    def syndromes(self, cw):
        self.write(DATA, cw); self.write(DATA + 0x200, bytes(32))
        nz = self.call("seal_syndromes", DATA, DATA + 0x200)
        return list(self.read(DATA + 0x200, 32)), nz

    def decode(self, cw, addr=DATA):
        self.write(addr, cw)
        r = self.call("seal_decode", addr, maxinsn=20_000_000)
        r = r - (1 << 32) if r & 0x80000000 else r
        return self.read(addr, 152), r
