#!/usr/bin/env python3
import re
import sys


OPS = {}


def load_dump(path):
    cells = {}
    call_names = {}
    global_names = {}
    for line in open(path, "r", errors="replace"):
        m = re.match(r"\s+call\[(\d+)\] = (\S+)", line)
        if m:
            call_names[int(m.group(1))] = m.group(2)
            continue
        m = re.match(r"\s+global\[(\d+)\] = ", line)
        if m:
            global_names[int(m.group(1))] = line.strip()
            continue
        m = re.match(
            r"\s+(\d+) (\w+) dst=(\d+) a=(\d+) b=(\d+) imm=(-?\d+)(?: hi=(\d+))?(?: -> (\d+))?",
            line,
        )
        if m:
            pc = int(m.group(1))
            cells[pc] = (
                m.group(2),
                int(m.group(3)),
                int(m.group(4)),
                int(m.group(5)),
                int(m.group(6)),
                int(m.group(7)) if m.group(7) else 0,
            )
    return cells, call_names, global_names


class Fault(Exception):
    pass


class VM:
    def __init__(self, cells, call_names, base, array, lo, hi, max_depth=40):
        self.cells = cells
        self.call_names = call_names
        self.mem = {}
        self.base = base
        self.lo = base - 0x1000
        self.hi = base + 0x4000
        for i, v in enumerate(array):
            self.store(base + 4 * i, v, 4)
        self.depth = 0
        self.max_depth = max_depth
        self.trace = []
        self.steps = 0
        self.regs = [0] * 256
        self.regs[0] = base
        self.regs[1] = lo
        self.regs[2] = hi
    def load(self, addr, size):
        if addr < self.lo or addr + size > self.hi:
            raise Fault("wild read at 0x%x" % addr)
        v = 0
        for i in range(size):
            v |= self.mem.get(addr + i, 0) << (8 * i)
        return v

    def store(self, addr, value, size):
        if addr < self.lo or addr + size > self.hi:
            raise Fault("wild write at 0x%x" % addr)
        for i in range(size):
            self.mem[addr + i] = (value >> (8 * i)) & 0xFF

    def se(self, v, width):
        if width >= 64:
            return v
        sign = 1 << (width - 1)
        v &= (1 << width) - 1
        return (v ^ sign) - sign

    def u(self, v, width):
        if width >= 64:
            return v
        return v & ((1 << width) - 1)

    def call(self, index, args):
        if index != 0:
            raise Fault("unsupported call target %d" % index)
        if self.depth >= self.max_depth:
            raise Fault("max recursion depth reached")
        child = VM(self.cells, self.call_names, self.base, [], 0, 0, self.max_depth)
        child.mem = self.mem
        child.lo, child.hi = self.lo, self.hi
        child.depth = self.depth + 1
        child.regs = [0] * 256
        child.regs[0] = args[0]
        child.regs[1] = args[1]
        child.regs[2] = args[2]
        r = child.run()
        self.mem = child.mem
        return r

    def log(self, pc, text):
        self.trace.append("pc=%d %s" % (pc, text))
        if len(self.trace) > 300:
            self.trace.pop(0)

    def run(self, limit=200000):
        pc = 0
        while True:
            if self.steps > limit:
                raise Fault("step limit reached")
            if pc not in self.cells:
                raise Fault("pc %d out of range" % pc)
            op, dst, a, b, imm, hi = self.cells[pc]
            self.steps += 1
            self.log(pc, "%s dst=%d a=%d b=%d imm=%d" % (op, dst, a, b, imm))
            pc += 1
            if op == "CONST":
                self.regs[dst] = imm & 0xFFFFFFFF
            elif op == "CONST64":
                self.regs[dst] = (imm & 0xFFFFFFFF) | (hi << 32)
                pc += 1
            elif op == "MOV":
                self.regs[dst] = self.regs[a]
            elif op == "ADD":
                self.regs[dst] = (self.regs[a] + self.regs[b]) & 0xFFFFFFFFFFFFFFFF
            elif op == "SUB":
                self.regs[dst] = (self.regs[a] - self.regs[b]) & 0xFFFFFFFFFFFFFFFF
            elif op == "MUL":
                self.regs[dst] = (self.regs[a] * self.regs[b]) & 0xFFFFFFFFFFFFFFFF
            elif op == "SDIV":
                x, y = self.se(self.regs[a], 64), self.se(self.regs[b], 64)
                self.regs[dst] = self.u(int(x / y), 64) if y else 0xFFFFFFFFFFFFFFFF
            elif op == "AND":
                self.regs[dst] = self.regs[a] & self.regs[b]
            elif op == "OR":
                self.regs[dst] = self.regs[a] | self.regs[b]
            elif op == "XOR":
                self.regs[dst] = self.regs[a] ^ self.regs[b]
            elif op == "SHL":
                self.regs[dst] = (self.regs[a] << (self.regs[b] & 63)) & 0xFFFFFFFFFFFFFFFF
            elif op == "SEXT":
                self.regs[dst] = self.u(self.se(self.regs[a], imm), 64)
            elif op == "TRUNC":
                self.regs[dst] = self.u(self.regs[a], imm)
            elif op == "ICMP_EQ":
                self.regs[dst] = 1 if self.regs[a] == self.regs[b] else 0
            elif op == "ICMP_SLT":
                self.regs[dst] = 1 if self.se(self.regs[a], 32) < self.se(self.regs[b], 32) else 0
            elif op == "ICMP_SGT":
                self.regs[dst] = 1 if self.se(self.regs[a], 32) > self.se(self.regs[b], 32) else 0
            elif op == "GEP_REG":
                self.regs[dst] = (self.regs[a] + (self.se(self.regs[b], 32) * imm)) & 0xFFFFFFFFFFFFFFFF
            elif op == "GEP_IMM":
                self.regs[dst] = (self.regs[a] + imm) & 0xFFFFFFFFFFFFFFFF
            elif op == "LOAD32":
                self.regs[dst] = self.load(self.regs[a], 4)
            elif op == "STORE32":
                self.store(self.regs[a], self.regs[b], 4)
            elif op == "JMP":
                pc = imm
            elif op == "CONDBR":
                if self.regs[a]:
                    pc = imm
            elif op == "LOADADDR":
                self.regs[dst] = 0xDEAD0000 + imm
            elif op == "CALL":
                args = [self.regs[241 + i] for i in range(3)]
                self.log(pc - 1, "  call args=%s" % args)
                self.regs[dst] = self.call(imm, args)
            elif op == "RET":
                return self.regs[a] if a != 255 else 0
            else:
                raise Fault("unimplemented op %s" % op)

    def dump(self):
        r = self.regs
        print("r0=0x%x r1=%d r2=%d r3=%d r4=%d r5=%d r6=%d r7=%d r9=%d" % (
            r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[9]))
        print("r10=%d r11=%d r12=%d r13=%d r15=%d r17=%d r19=%d r21=%d r23=%d" % (
            r[10], r[11], r[12], r[13], r[15], r[17], r[19], r[21], r[23]))
        print("r26=%d r31=%d r32=%d r35=%d r36=%d r37=%d r38=%d r39=%d" % (
            r[26], r[31], r[32], r[35], r[36], r[37], r[38], r[39]))
        print("array:", [self.load(self.base + 4 * i, 4) for i in range(8)])
        print("last trace:")
        for t in self.trace[-25:]:
            print("  " + t)


def main():
    path = sys.argv[1]
    base = 0x100000
    array = [5, 3, 9, 1, 7, 2, 8, 4]
    cells, call_names, global_names = load_dump(path)
    vm = VM(cells, call_names, base, array, 0, 7)
    try:
        vm.run()
        print("returned normally")
    except Fault as f:
        print("FAULT: %s" % f)
    vm.dump()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
