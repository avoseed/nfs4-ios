"""Differential test: real x86 CPU vs recompiled C, using instructions taken from the game.

    python difftest.py <pe> [--per-shape N] [--seed S] [--only mnem,...]
"""
import argparse, collections, os, random, struct, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import x86rec as X
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_OP_REG

REGION, DATA_END, STACK, FLAGS_OUT, CODE = 0x10000000, 0x1007F000, 0x10070000, 0x1007F000, 0x10100000
SKIP = {'call', 'ret', 'jmp', 'int', 'int3', 'hlt', 'iretd', 'cpuid', 'rdtsc', 'les', 'lds', 'in', 'out', 'popfd',
        'popf', 'leave', 'enter', 'into', 'bound', 'emms', 'movq', 'movd', 'loop', 'loope', 'loopne', 'jecxz',
        'std', 'cld', 'cli', 'sti', 'nop', 'wait', 'aas', 'aaa', 'daa', 'das', 'aam', 'aad', 'arpl', 'salc',
        'insb', 'insd', 'outsb', 'outsd', 'lock', 'xlatb', 'pushal', 'popal', 'ljmp', 'lcall', 'retf'}
GPR = ['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi']
CF, PF, AF, ZF, SF, OF = 1, 4, 16, 64, 128, 2048
CMP_FLAGS = CF | PF | ZF | SF | OF


def flag_mask(i):
    m = i.mnemonic.split()[-1]
    if m in ('mul', 'imul'):
        return CF | OF
    if m in ('div', 'idiv'):
        return 0
    if m in ('bsf', 'bsr'):
        return ZF
    if m in ('bt', 'bts', 'btr', 'btc'):
        return CF
    if m in X.SHIFTS:
        cnt = i.operands[-1] if len(i.operands) >= 2 else None
        one = cnt is None or (cnt.type == X86_OP_IMM and (cnt.imm & 31) == 1)
        if m in ('rol', 'ror', 'rcl', 'rcr'):
            return (CF | OF) if one else CF
        return CMP_FLAGS if one else CMP_FLAGS & ~OF
    return CMP_FLAGS


def interesting():
    return random.choice([0, 1, 2, 0x7F, 0x80, 0xFF, 0x100, 0x7FFF, 0x8000, 0xFFFF, 0x7FFFFFFF, 0x80000000,
                          0xFFFFFFFF, 0xFFFFFFFE, random.getrandbits(32), random.getrandbits(32), random.getrandbits(8),
                          random.getrandbits(16)])


def make_regs(i):
    regs = [interesting() for _ in range(8)]
    regs[4] = STACK
    m = i.mnemonic
    ptr_regs = set()
    for op in i.operands:
        if op.type == X86_OP_MEM:
            if op.mem.base:
                ptr_regs.add(i.reg_name(op.mem.base))
            if op.mem.index:
                ptr_regs.add(i.reg_name(op.mem.index))
    if any(s in m for s in ('movs', 'stos', 'lods', 'scas', 'cmps')) and not m.startswith('movsx'):
        ptr_regs |= {'esi', 'edi'}
        regs[1] = random.randint(0, 24)
    for r in ptr_regs:
        if r in GPR and r != 'esp':
            regs[GPR.index(r)] = random.randrange(0x10010000, 0x10060000) & ~random.choice([0, 1, 3])
    base = m.split()[-1]
    if base in ('div', 'idiv'):
        sz = i.operands[0].size
        regs[2] = 0 if sz >= 2 else regs[2]
        if sz == 1:
            regs[0] = (regs[0] & 0xFFFF0000) | random.randint(0, 127)
        elif sz == 2:
            regs[0] = (regs[0] & 0xFFFF0000) | random.randint(0, 0x7FFF)
        else:
            regs[0] = random.randint(0, 0x7FFFFFFF)
        op = i.operands[0]
        if op.type == X86_OP_REG:
            r = i.reg_name(op.reg)
            full = X.R16.get(r) or X.R8L.get(r) or X.R8H.get(r) or r
            k = GPR.index(full)
            if r in X.R8H:
                regs[k] = (regs[k] & 0xFFFF00FF) | (random.randint(1, 255) << 8)
            else:
                regs[k] = (regs[k] & ~((1 << (8 * op.size)) - 1) & 0xFFFFFFFF) | random.randint(1, (1 << (8 * op.size)) - 1)
            if full in ('eax', 'edx'):
                return None
    return regs


def ea_ok(i, regs):
    for op in i.operands:
        if op.type != X86_OP_MEM:
            continue
        mem = op.mem
        if mem.segment:
            return False
        a = mem.disp
        if mem.base:
            a += regs[GPR.index(i.reg_name(mem.base))] if i.reg_name(mem.base) in GPR else 1 << 40
        if mem.index:
            a += regs[GPR.index(i.reg_name(mem.index))] * mem.scale if i.reg_name(mem.index) in GPR else 1 << 40
        a &= 0xFFFFFFFF
        if not (REGION <= a < DATA_END - 64):
            return False
    return True


def collect(pe_path, per_shape, only):
    mod = X.Module(pe_path)
    funcs, tables = X.discover(mod)
    shapes = collections.defaultdict(list)
    for a, i in mod.cache.items():
        m = i.mnemonic
        base = m.split()[-1]
        if base in SKIP or m.startswith('j') or m.startswith('f') or m.startswith('p') and m not in ('push', 'pop'):
            continue
        if only and base not in only:
            continue
        # instructions that load esp with arbitrary values would leave the test without a stack
        if i.operands and i.operands[0].type == X86_OP_REG and i.reg_name(i.operands[0].reg) in ('esp', 'sp') \
                and not (base in ('add', 'sub') and i.operands[1].type == X86_OP_IMM and abs(i.operands[1].imm) < 0x1000):
            continue
        if base == 'xchg' and any(o.type == X86_OP_REG and i.reg_name(o.reg) == 'esp' for o in i.operands):
            continue
        # segment register loads would break the host process
        if i.operands and i.operands[0].type == X86_OP_REG and i.reg_name(i.operands[0].reg) in X.SEGS:
            continue
        key = (m, tuple(b for b in bytes(i.bytes) if b in (0xF2, 0xF3, 0x66)), tuple((o.type, o.size, i.reg_name(o.reg) if o.type == X86_OP_REG else '') for o in i.operands))
        if len(shapes[key]) < per_shape and bytes(i.bytes) not in [x.bytes for x in shapes[key]]:
            shapes[key].append(i)
    return [i for v in shapes.values() for i in v]


class FakeModule:
    def __init__(self, code):
        self.base = CODE
        self.img = bytearray(code)
        self.code_ranges = [(CODE, CODE + len(code))]
        self.md = Cs(CS_ARCH_X86, CS_MODE_32)
        self.md.detail = True
        self.cache, self.imports, self.relocs, self.exports = {}, {}, {}, {}

    in_code = X.Module.in_code
    insn = X.Module.insn
    u32 = X.Module.u32


def zig():
    sys.path.insert(0, os.path.dirname(HERE))
    import build_host
    return build_host.zig()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('pe')
    ap.add_argument('--per-shape', type=int, default=2)
    ap.add_argument('--reps', type=int, default=6)
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--only')
    args = ap.parse_args()
    random.seed(args.seed)
    only = set(args.only.split(',')) if args.only else None
    insns = collect(args.pe, args.per_shape, only)
    tests = []
    for i in insns:
        for _ in range(args.reps):
            for _ in range(20):
                regs = make_regs(i)
                if regs and ea_ok(i, regs):
                    break
            else:
                continue
            fl = random.getrandbits(1) * CF | random.getrandbits(1) * ZF | random.getrandbits(1) * SF | \
                 random.getrandbits(1) * OF | random.getrandbits(1) * PF
            if any(s in i.mnemonic for s in ('movs', 'stos', 'lods', 'scas', 'cmps')) and random.random() < 0.3:
                fl |= 0x400
            code = (bytes(i.bytes) + b'\x9c\x8f\x05' + struct.pack('<I', FLAGS_OUT) + b'\x89\x25' + struct.pack('<I', FLAGS_OUT + 4) +
                    b'\xbc' + struct.pack('<I', FLAGS_OUT + 16) + b'\xc3')
            tests.append((i, code, regs, fl))
    print('%d instruction shapes, %d tests' % (len(insns), len(tests)))

    # recompile every test with the real emitter
    blob = bytearray()
    entries = []
    for _, code, _, _ in tests:
        entries.append(CODE + len(blob))
        blob += code + b'\xcc' * (64 - len(code))
    fm = FakeModule(blob)
    em = X.Emitter(fm, 'T', set(entries), {}, {})
    em.lines = ['#define RT_FLAGS_FROM_CPU', '#include "rt.h"', '']
    for e in entries:
        em.function(X.Function(fm, e, set(entries), {}))
    em.lines.append('void (*const dt_funcs[])(Cpu *) = {%s};' % ','.join(em.fname(e) for e in entries))
    em.lines.append('const uint32_t dt_count = %d;' % len(entries))
    build = os.path.join(HERE, 'build')
    os.makedirs(build, exist_ok=True)
    open(os.path.join(build, 'dt_gen.c'), 'w').write('\n'.join(em.lines))

    with open(os.path.join(build, 'tests.bin'), 'wb') as f:
        f.write(struct.pack('<I', len(tests)))
        for k, (_, code, regs, fl) in enumerate(tests):
            f.write(code.ljust(48, b'\xcc') + struct.pack('<8I', *regs) + struct.pack('<II', fl, k + 1))

    z = zig()
    rt = os.path.join(os.path.dirname(HERE), 'runtime')
    nat = os.path.join(build, 'native_runner.exe')
    rec = os.path.join(build, 'rec_runner.exe')
    subprocess.run([z, 'cc', '-target', 'x86-windows-gnu', '-O1', '-Wl,--image-base,0x30000000', '-I', HERE,
                    os.path.join(HERE, 'native_runner.c'), '-o', nat], check=True)
    subprocess.run([z, 'cc', '-target', 'x86_64-windows-gnu', '-O1', '-w', '-fno-strict-aliasing', '-fwrapv', '-mllvm', '-fixup-byte-word-insts=false', '-I', HERE,
                    '-I', rt, os.path.join(HERE, 'rec_runner.c'), os.path.join(build, 'dt_gen.c'), '-o', rec], check=True)
    subprocess.run([nat, os.path.join(build, 'tests.bin'), os.path.join(build, 'native.bin')], check=True)
    subprocess.run([rec, os.path.join(build, 'tests.bin'), os.path.join(build, 'rec.bin')], check=True)

    rsize = 8 * 4 + 4 + 4 + 48 * 4 + 48
    nat_r = open(os.path.join(build, 'native.bin'), 'rb').read()
    rec_r = open(os.path.join(build, 'rec.bin'), 'rb').read()
    bad = collections.defaultdict(list)
    faults = collections.Counter()
    for k, (i, code, regs, fl) in enumerate(tests):
        a = nat_r[k * rsize:(k + 1) * rsize]
        b = rec_r[k * rsize:(k + 1) * rsize]
        ra, rb = struct.unpack_from('<8I', a), struct.unpack_from('<8I', b)
        fa, fb = struct.unpack_from('<I', a, 32)[0], struct.unpack_from('<I', b, 32)[0]
        def changes(r):
            n = struct.unpack_from('<I', r, 36)[0]
            addrs = struct.unpack_from('<48I', r, 40)
            vals = r[40 + 192:40 + 192 + 48]
            return {addrs[j]: vals[j] for j in range(n) if not (STACK - 8 <= addrs[j] < STACK)}
        if struct.unpack_from('<I', b, 36)[0] == 0xFFFFFFFE:
            faults['rec-trap:' + i.mnemonic] += 1
            continue
        if struct.unpack_from('<I', a, 36)[0] == 0xFFFFFFFF:
            faults[i.mnemonic] += 1
            continue
        mask = flag_mask(i)
        problems = []
        if ra != rb:
            problems.append('regs ' + ' '.join('%s:%08X/%08X' % (GPR[j], ra[j], rb[j]) for j in range(8) if ra[j] != rb[j]))
        if (fa & mask) != (fb & mask):
            problems.append('flags native=%03X rec=%03X (mask %03X)' % (fa & mask, fb & mask, mask))
        ca, cb = changes(a), changes(b)
        if ca != cb:
            problems.append('memory native=%s rec=%s' % (sorted(ca.items())[:6], sorted(cb.items())[:6]))
        if problems:
            bad[(i.mnemonic, i.op_str)].append((regs, fl, problems))
    print('native faults (skipped):', dict(faults))
    print('%d/%d tests differ, %d instruction forms' % (sum(len(v) for v in bad.values()), len(tests), len(bad)))
    for (m, o), lst in sorted(bad.items()):
        regs, fl, problems = lst[0]
        print('  %-8s %-34s x%d  in:%s fl=%03X' % (m, o, len(lst), ' '.join('%08X' % r for r in regs), fl))
        for p in problems:
            print('        ' + p)


if __name__ == '__main__':
    main()
