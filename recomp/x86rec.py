"""Static recompiler: 32-bit x86 PE (Watcom-built NFS3 binaries) -> portable C.

Model
-----
* Guest memory is a flat 32-bit space at host pointer MEM (see runtime/rt.h).
* Every discovered function entry becomes `void F_<addr>(Cpu *c)`. Guest
  registers live in C locals inside a function and are synced to `c` around
  calls, so the host compiler can keep them in registers.
* Guest `call` pushes the real return address on the guest stack and calls the
  C function; guest `ret` pops it into c->ret_to and returns. Callers check
  c->ret_to so unusual control flow is caught instead of silently diverging.
* Flags are function-local and only computed when a later instruction reads
  them (backward liveness), so `cmp/jcc` becomes a plain C comparison.
* Imports are called by name through hle_call(); IAT slots are also filled
  with magic addresses so indirect calls through copied pointers still work.

Usage: python x86rec.py <pe> <outdir> <prefix> [--base 0xADDR] [--seeds file]
"""
import argparse, collections, os, struct, sys
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_OP_REG

ALL_FLAGS = frozenset('CPAZSO')
R32 = ['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi']
R16 = {'ax': 'eax', 'cx': 'ecx', 'dx': 'edx', 'bx': 'ebx', 'sp': 'esp', 'bp': 'ebp', 'si': 'esi', 'di': 'edi'}
R8L = {'al': 'eax', 'cl': 'ecx', 'dl': 'edx', 'bl': 'ebx'}
R8H = {'ah': 'eax', 'ch': 'ecx', 'dh': 'edx', 'bh': 'ebx'}
MASK = {1: '0xFFu', 2: '0xFFFFu', 4: '0xFFFFFFFFu'}
SIGN = {1: '0x80u', 2: '0x8000u', 4: '0x80000000u'}
UT = {1: 'uint8_t', 2: 'uint16_t', 4: 'uint32_t'}
ST = {1: 'int8_t', 2: 'int16_t', 4: 'int32_t'}
BITS = {1: 8, 2: 16, 4: 32}
# Win32 flat-model selector values
SEGS = {'cs': '0x1Bu', 'ds': '0x23u', 'es': '0x23u', 'ss': '0x23u', 'fs': '0x3Bu', 'gs': '0x00u'}

JCC = {
    'jo': 'of', 'jno': '!of', 'jb': 'cf', 'jae': '!cf', 'je': 'zf', 'jne': '!zf',
    'jbe': '(cf|zf)', 'ja': '!(cf|zf)', 'js': 'sf', 'jns': '!sf', 'jp': 'pf', 'jnp': '!pf',
    'jl': '(sf!=of)', 'jge': '(sf==of)', 'jle': '(zf|(sf!=of))', 'jg': '(!zf&&(sf==of))',
}
JCC_USES = {
    'jo': 'O', 'jno': 'O', 'jb': 'C', 'jae': 'C', 'je': 'Z', 'jne': 'Z', 'jbe': 'CZ', 'ja': 'CZ',
    'js': 'S', 'jns': 'S', 'jp': 'P', 'jnp': 'P', 'jl': 'SO', 'jge': 'SO', 'jle': 'ZSO', 'jg': 'ZSO',
}
SETCC = {'set' + k[1:]: v for k, v in JCC.items()}
SETCC_USES = {'set' + k[1:]: v for k, v in JCC_USES.items()}
CMOV = {}

ARITH_DEFS = {
    'add': ALL_FLAGS, 'sub': ALL_FLAGS, 'cmp': ALL_FLAGS, 'adc': ALL_FLAGS, 'sbb': ALL_FLAGS,
    'neg': ALL_FLAGS, 'and': ALL_FLAGS, 'or': ALL_FLAGS, 'xor': ALL_FLAGS, 'test': ALL_FLAGS,
    'inc': frozenset('PAZSO'), 'dec': frozenset('PAZSO'),
    'imul': frozenset('CO'), 'mul': frozenset('CO'),
    'div': ALL_FLAGS, 'idiv': ALL_FLAGS,
    'bt': frozenset('C'), 'bts': frozenset('C'), 'btc': frozenset('C'), 'btr': frozenset('C'),
    'bsf': frozenset('Z'), 'bsr': frozenset('Z'),
    'sahf': frozenset('CPAZS'), 'popfd': ALL_FLAGS, 'popal': frozenset(),
    'clc': frozenset('C'), 'stc': frozenset('C'),
    'cmpsb': ALL_FLAGS, 'cmpsw': ALL_FLAGS, 'cmpsd': ALL_FLAGS,
    'scasb': ALL_FLAGS, 'scasw': ALL_FLAGS, 'scasd': ALL_FLAGS,
}
SHIFTS = {'shl', 'sal', 'shr', 'sar', 'rol', 'ror', 'rcl', 'rcr', 'shld', 'shrd'}


class Fail(Exception):
    pass


# ---------------------------------------------------------------- module model

class Module:
    def __init__(self, path, base=None):
        self.pe = pefile.PE(path)
        if base is not None and base != self.pe.OPTIONAL_HEADER.ImageBase:
            self.pe.relocate_image(base)
            self.pe.OPTIONAL_HEADER.ImageBase = base
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.img = bytearray(self.pe.get_memory_mapped_image())
        size = self.pe.OPTIONAL_HEADER.SizeOfImage
        if len(self.img) < size:
            self.img += bytes(size - len(self.img))
        code = [s for s in self.pe.sections if s.Characteristics & 0x20]
        self.code_ranges = [(self.base + s.VirtualAddress,
                             self.base + s.VirtualAddress + max(s.SizeOfRawData, s.Misc_VirtualSize)) for s in code]
        self.md = Cs(CS_ARCH_X86, CS_MODE_32)
        self.md.detail = True
        self.cache = {}
        self.imports = {}       # iat slot address -> (dll, name)
        for imp in getattr(self.pe, 'DIRECTORY_ENTRY_IMPORT', []):
            for i in imp.imports:
                name = i.name.decode() if i.name else '#%d' % i.ordinal
                self.imports[i.address] = (imp.dll.decode().lower(), name)
        self.exports = {}
        if hasattr(self.pe, 'DIRECTORY_ENTRY_EXPORT'):
            for e in self.pe.DIRECTORY_ENTRY_EXPORT.symbols:
                self.exports[self.base + e.address] = e.name.decode() if e.name else '#%d' % e.ordinal
        self.relocs = {}        # site -> value
        for blk in getattr(self.pe, 'DIRECTORY_ENTRY_BASERELOC', []):
            for e in blk.entries:
                if e.type == 3:
                    site = self.base + e.rva
                    self.relocs[site] = self.u32(site)

    def in_code(self, a):
        return any(lo <= a < hi for lo, hi in self.code_ranges)

    def u32(self, a):
        o = a - self.base
        return struct.unpack_from('<I', self.img, o)[0]

    def insn(self, a):
        i = self.cache.get(a)
        if i is None:
            o = a - self.base
            try:
                i = next(self.md.disasm(bytes(self.img[o:o + 16]), a))
            except StopIteration:
                raise Fail('undecodable at %08x' % a)
            self.cache[a] = i
        return i


# ---------------------------------------------------------------- discovery

def is_terminator(i):
    m = i.mnemonic
    return m in ('ret', 'retf', 'jmp', 'hlt', 'iretd') or m.startswith('ret')


def branch_target(i):
    op = i.operands[0] if i.operands else None
    if op is not None and op.type == X86_OP_IMM:
        return op.imm & 0xFFFFFFFF
    return None


def jump_table(mod, i):
    """jmp dword ptr [disp + reg*4] with relocated entries -> list of targets. Watcom also emits
    [disp + reg] / [disp + reg*1] with the index already multiplied by 4 (e.g. 0x452648 in
    nfs3.exe, which loads it from a stack slot); the emitted switch works on the loaded target,
    so those forms are tables too as long as disp is a run of relocated code pointers."""
    if i.mnemonic != 'jmp' or not i.operands:
        return None
    op = i.operands[0]
    if op.type != X86_OP_MEM or not op.mem.disp:
        return None
    scaled = op.mem.index and not op.mem.base and op.mem.scale == 4
    offset = (op.mem.base and not op.mem.index) or (op.mem.index and not op.mem.base and op.mem.scale == 1)
    if not (scaled or offset):
        return None
    t = op.mem.disp & 0xFFFFFFFF
    out = []
    while t in mod.relocs and mod.in_code(mod.relocs[t]) and len(out) < 1024:
        out.append(mod.relocs[t])
        t += 4
    return out or None


def plausible(mod, a, limit=4000):
    """Code pointers from relocations may point at data; accept only addresses
    that decode cleanly until control leaves."""
    seen = 0
    while seen < limit:
        if not mod.in_code(a):
            return False
        try:
            i = mod.insn(a)
        except Fail:
            return False
        if i.mnemonic in ('int', 'int3', 'hlt', 'in', 'out', 'insb', 'insd', 'outsb', 'outsd', 'arpl', 'les', 'lds',
                          'into', 'bound', 'aaa', 'aas', 'daa', 'das', 'aam', 'aad', 'salc', 'sti', 'cli'):
            return False
        if is_terminator(i) or i.mnemonic == 'call':
            return True
        a += i.size
        seen += 1
    return True


def discover(mod, extra_seeds=()):
    seeds = set()
    ep = mod.base + mod.pe.OPTIONAL_HEADER.AddressOfEntryPoint
    if mod.in_code(ep) and mod.pe.OPTIONAL_HEADER.AddressOfEntryPoint:
        seeds.add(ep)
    seeds |= {a for a in mod.exports if mod.in_code(a)}
    seeds |= set(extra_seeds)
    reloc_code_targets = {(s, v) for s, v in mod.relocs.items() if mod.in_code(v)}
    for s, v in reloc_code_targets:
        if not mod.in_code(s) and plausible(mod, v):
            seeds.add(v)

    funcs = set()
    tables = {}
    insn_sites = {}          # address -> instruction covering it
    work = list(seeds)
    def walk():
        while work:
            f = work.pop()
            if f in funcs or not mod.in_code(f):
                continue
            funcs.add(f)
            stack = [f]
            seen = set()
            while stack:
                a = stack.pop()
                while mod.in_code(a) and a not in seen:
                    seen.add(a)
                    try:
                        i = mod.insn(a)
                    except Fail:
                        break
                    for k in range(i.size):
                        insn_sites[a + k] = a
                    m = i.mnemonic
                    if m == 'call':
                        t = branch_target(i)
                        if t is not None and mod.in_code(t):
                            work.append(t)
                    elif m == 'jmp' or (m.startswith('j') and m in JCC) or m in ('jecxz', 'loop', 'loope', 'loopne'):
                        t = branch_target(i)
                        if t is not None:
                            stack.append(t)
                        elif m == 'jmp':
                            tab = jump_table(mod, i)
                            if tab:
                                tables[a] = tab
                                stack.extend(tab)
                    if is_terminator(i):
                        break
                    a += i.size
    walk()
    # code-section relocations that are not part of a jump table and sit inside
    # an instruction are `push offset fn` / `mov reg, offset fn`: callbacks.
    table_sites = set()
    for ia, tab in tables.items():
        d = mod.insn(ia).operands[0].mem.disp & 0xFFFFFFFF
        table_sites |= {d + 4 * k for k in range(len(tab))}
    for _ in range(3):
        added = False
        for s, v in reloc_code_targets:
            if mod.in_code(s) and s not in table_sites and v not in funcs:
                if plausible(mod, v):
                    work.append(v)
                    added = True
        if not added:
            break
        walk()
    # a [reg + table] jump whose entries all start functions is a tail call through a table of
    # functions (0x4B24E7: 80 of them), not a switch: rt_call_indirect handles it, and treating it
    # as a switch would copy every one of those functions into the caller
    for ia in list(tables):
        op = mod.insn(ia).operands[0]
        if not (op.mem.index and not op.mem.base and op.mem.scale == 4) and all(t in funcs for t in tables[ia]):
            del tables[ia]
    return funcs, tables


# ---------------------------------------------------------------- functions

class Function:
    flag_returning = set()    # functions whose callers read the flags they leave behind

    def __init__(self, mod, entry, funcs, tables):
        self.mod = mod
        self.entry = entry
        self.insns = {}
        self.succ = {}
        self.labels = {entry}
        stack = [entry]
        while stack:
            a = stack.pop()
            while True:
                if a in self.insns:
                    self.labels.add(a)
                    break
                try:
                    i = mod.insn(a) if mod.in_code(a) else None
                except Fail:
                    i = None
                if i is None:
                    self.insns[a] = None   # falls off the code section / bad decode
                    self.succ[a] = []
                    break
                self.insns[a] = i
                m = i.mnemonic
                nxt = a + i.size
                succ = []
                if m == 'jmp':
                    t = branch_target(i)
                    if t is not None:
                        if t in funcs and t != entry:
                            succ = []        # tail call
                        else:
                            succ = [t]
                            self.labels.add(t)
                            stack.append(t)
                    elif a in tables:
                        succ = list(dict.fromkeys(tables[a]))
                        for t in succ:
                            self.labels.add(t)
                            stack.append(t)
                    self.succ[a] = succ
                    break
                if m in JCC or m in ('jecxz', 'loop', 'loope', 'loopne'):
                    t = branch_target(i)
                    succ = [nxt]
                    if t in funcs and t != entry:
                        pass                 # conditional tail call, handled at emit
                    else:
                        succ.append(t)
                        self.labels.add(t)
                        stack.append(t)
                    self.succ[a] = succ
                    self.labels.add(nxt)
                    a = nxt
                    continue
                if m.startswith('ret') or m in ('hlt', 'int3') or (m == 'int'):
                    self.succ[a] = []
                    break
                self.succ[a] = [nxt]
                a = nxt
        self.order = sorted(self.insns)
        # any fallthrough that is not the next emitted instruction needs a label
        for k, a in enumerate(self.order):
            i = self.insns[a]
            if i is None:
                continue
            nxt = a + i.size
            if nxt in self.succ.get(a, []) and (k + 1 >= len(self.order) or self.order[k + 1] != nxt):
                self.labels.add(nxt)

    # ---- flag liveness
    def flag_effects(self, i):
        """(uses, defs, killing) for one instruction."""
        if i is None:
            return set(), set(), True
        m = i.mnemonic
        base = m.split()[-1] if m.startswith('rep') else m
        if m in JCC:
            return set(JCC_USES[m]), set(), True
        if m in SETCC:
            return set(SETCC_USES[m]), set(), True
        if m in ('adc', 'sbb', 'rcl', 'rcr', 'cmc'):
            return {'C'}, set(ALL_FLAGS if m in ('adc', 'sbb') else 'CO' if m != 'cmc' else 'C'), m in ('adc', 'sbb', 'cmc')
        if m in ('pushfd', 'pushf', 'lahf'):
            return set(ALL_FLAGS), set(), True
        if m.startswith('ret') and self.entry in Function.flag_returning:
            return set(ALL_FLAGS), set(), True
        if m in ('call',) or m.startswith('ret') or (m == 'jmp' and not self.succ.get(i.address)):
            return set(), set(ALL_FLAGS), True
        if m in SHIFTS:
            cnt = i.operands[-1] if len(i.operands) >= 2 else None
            const = cnt is not None and cnt.type == X86_OP_IMM and (cnt.imm & 31) != 0
            defs = set('CO') if m in ('rol', 'ror') else set(ALL_FLAGS)
            return set(), defs, const
        if base in ARITH_DEFS:
            if m.startswith('rep'):
                return set(), set(ARITH_DEFS[base]), False   # rep with ecx==0 leaves flags
            return set(), set(ARITH_DEFS[base]), True
        if m.startswith('f') and m in ('fcomi', 'fcomip', 'fucomi', 'fucomip'):
            return set(), set('CPZ'), True
        return set(), set(), True

    def liveness(self):
        eff = {a: self.flag_effects(self.insns[a]) for a in self.order}
        live_in = {a: set() for a in self.order}
        live_out = {a: set() for a in self.order}
        changed = True
        while changed:
            changed = False
            for a in reversed(self.order):
                out = set()
                for s in self.succ.get(a, []):
                    if s in live_in:
                        out |= live_in[s]
                uses, defs, kill = eff[a]
                inn = uses | ((out - defs) if kill else out)
                if out != live_out[a] or inn != live_in[a]:
                    live_out[a] = out
                    live_in[a] = inn
                    changed = True
        self.live_out = live_out


# ---------------------------------------------------------------- C emission

def h(a):
    return '0x%08Xu' % (a & 0xFFFFFFFF)


class Emitter:
    def __init__(self, mod, prefix, funcs, tables, import_ids):
        self.mod = mod
        self.prefix = prefix
        self.funcs = funcs
        self.tables = tables
        self.import_ids = import_ids
        self.lines = []
        self.unsupported = collections.Counter()

    def fname(self, a):
        return '%s_%08X' % (self.prefix, a)

    def out(self, s):
        self.lines.append(s)

    # ---- operands
    def addr(self, i, op):
        mem = op.mem
        parts = []
        if mem.base:
            r = i.reg_name(mem.base)
            parts.append('(uint16_t)' + R16[r] if r in R16 else r)
        if mem.index:
            r = i.reg_name(mem.index)
            r = '(uint16_t)' + R16[r] if r in R16 else r
            parts.append('%s*%d' % (r, mem.scale) if mem.scale != 1 else r)
        if mem.disp or not parts:
            parts.append(h(mem.disp))
        e = '+'.join(parts)
        if i.addr_size == 2:
            e = '(uint16_t)(' + e + ')'
        seg = i.reg_name(mem.segment) if mem.segment else None
        if seg == 'fs':
            e = 'c->fs_base+' + e
        elif seg == 'gs':
            e = 'c->gs_base+' + e
        return '(uint32_t)(%s)' % e

    def rd(self, i, op, size=None):
        size = size or op.size
        if op.type == X86_OP_REG:
            r = i.reg_name(op.reg)
            if r in R32:
                return r
            if r in R16:
                return '(uint16_t)' + R16[r]
            if r in R8L:
                return '(uint8_t)' + R8L[r]
            if r in R8H:
                return '(uint8_t)(%s>>8)' % R8H[r]
            if r in SEGS:
                return SEGS[r]
            raise Fail('reg ' + r)
        if op.type == X86_OP_IMM:
            return h(op.imm & {1: 0xFF, 2: 0xFFFF, 4: 0xFFFFFFFF}[size])
        if op.type == X86_OP_MEM:
            return 'RD%d(%s)' % (BITS[size], self.addr(i, op))
        raise Fail('operand')

    def wr(self, i, op, val, size=None):
        size = size or op.size
        if op.type == X86_OP_REG:
            r = i.reg_name(op.reg)
            if r in R32:
                return '%s = (uint32_t)(%s);' % (r, val)
            if r in R16:
                g = R16[r]
                return '%s = (%s & 0xFFFF0000u) | (uint16_t)(%s);' % (g, g, val)
            if r in R8L:
                g = R8L[r]
                return '%s = (%s & 0xFFFFFF00u) | (uint8_t)(%s);' % (g, g, val)
            if r in R8H:
                g = R8H[r]
                return '%s = (%s & 0xFFFF00FFu) | ((uint32_t)(uint8_t)(%s) << 8);' % (g, g, val)
            if r in SEGS:
                return '(void)(%s); /* segment load ignored (flat model) */' % val
            raise Fail('wreg ' + r)
        if op.type == X86_OP_MEM:
            return 'WR%d(%s, %s);' % (BITS[size], self.addr(i, op), val)
        raise Fail('write operand')

    def szf(self, live, size, r='r'):
        s = []
        if 'Z' in live:
            s.append('zf = (%s == 0);' % r)
        if 'S' in live:
            s.append('sf = ((%s & %s) != 0);' % (r, SIGN[size]))
        if 'P' in live:
            s.append('pf = PARITY(%s);' % r)
        if 'A' in live:
            s.append('af = 0;')
        return ' '.join(s)

    # ---- control flow helpers
    def call_to(self, target_expr, ret_addr, direct=None, tail=False):
        """Emit a guest call (or tail jump when tail=True)."""
        o = []
        if not tail:
            o.append('esp -= 4; WR32(esp, %s);' % h(ret_addr))
        o.append('SYNC_OUT;')
        if direct is not None:
            o.append('%s(c);' % direct)
        else:
            o.append('rt_call_indirect(c, %s);' % target_expr)
        if tail:
            o.append('return;')
        else:
            o.append('SYNC_IN; if (c->ret_to != %s) rt_bad_return(c, %s);' % (h(ret_addr), h(ret_addr)))
        return ' '.join(o)

    def goto_or_tail(self, fn, t, ret_addr):
        if t in fn.labels and t in fn.insns:
            return 'goto L_%08X;' % t
        if t in self.funcs:
            return self.call_to(None, 0, direct=self.fname(t), tail=True)
        return self.call_to(h(t), 0, tail=True)

    # ---- function
    def function(self, fn):
        fn.liveness()
        # Loop headers get a compiler barrier so polling loops re-read guest memory that other
        # threads change (otherwise the C compiler may hoist the load out of the loop).
        loop_heads = set()
        for a in fn.order:
            i = fn.insns[a]
            if i is None:
                continue
            for s in fn.succ.get(a, []):
                if s <= a and s in fn.labels:
                    loop_heads.add(s)
        self.out('void %s(Cpu *c) {' % self.fname(fn.entry))
        self.out('  REGS_DECL; FLAGS_DECL; SYNC_IN; RT_ENTER(%s);' % h(fn.entry))
        # Observed in paired menu/race traces for this exact Modern Patch build.
        # This only tells the host which overlay to show; guest state is unchanged.
        if self.prefix == 'nfs4' and self.mod.pe.FILE_HEADER.TimeDateStamp == 0x57334341:
            if fn.entry == 0x00491990: self.out('  hle_frontend_observed(1);')
            if fn.entry == 0x0043C260: self.out('  hle_frontend_observed(0);')
            if fn.entry == 0x0051A710: self.out('  eax = hle_glide_set_mode(eax, RD32(esp));')
            if fn.entry == 0x0051A390: self.out('  hle_glide_prepare_modes();')

        if fn.order[0] != fn.entry:
            self.out('  goto L_%08X;' % fn.entry)
        prev_fallthrough = None
        for idx, a in enumerate(fn.order):
            i = fn.insns[a]
            if prev_fallthrough is not None and prev_fallthrough != a:
                self.out('  goto L_%08X;' % prev_fallthrough)
            if a in fn.labels:
                self.out('L_%08X:%s' % (a, ' RT_BARRIER();' if a in loop_heads else ''))
            if (self.prefix == 'nfs4' and a == 0x0048784A and i is not None
                    and self.mod.pe.FILE_HEADER.TimeDateStamp == 0x57334341):
                self.out('  hle_glide_select_race_entry();')
            if i is None:
                self.out('  rt_trap(c, %s, "fell off code");' % h(a))
                prev_fallthrough = None
                continue
            try:
                code = self.insn(fn, i)
            except Fail as e:
                self.unsupported[i.mnemonic] += 1
                code = 'rt_trap(c, %s, "unsupported: %s %s");' % (h(a), i.mnemonic, i.op_str.replace('"', ''))
            self.out('  /* %08X %s %s */ %s' % (a, i.mnemonic, i.op_str, code))
            succ = fn.succ.get(a, [])
            nxt = a + i.size
            prev_fallthrough = nxt if nxt in succ and not (i.mnemonic == 'jmp') else None
        if prev_fallthrough is not None:
            self.out('  goto L_%08X;' % prev_fallthrough)
        self.out('}')
        self.out('')

    # ---- single instruction
    def insn(self, fn, i):
        m = i.mnemonic
        ops = i.operands
        live = fn.live_out.get(i.address, set())
        nxt = i.address + i.size

        if m in ('nop', 'wait', 'fwait', 'emms') or m.startswith('nop'):
            return ';'

        # ---------- moves
        if m == 'mov':
            return self.wr(i, ops[0], self.rd(i, ops[1], ops[0].size))
        if m == 'movzx':
            return self.wr(i, ops[0], self.rd(i, ops[1]))
        if m == 'movsx':
            return self.wr(i, ops[0], '(uint32_t)(int32_t)(%s)(%s)' % (ST[ops[1].size], self.rd(i, ops[1])))
        if m == 'lea':
            mem = ops[1].mem
            return self.wr(i, ops[0], self.addr_noseg(i, ops[1]))
        if m == 'xchg':
            a, b = ops
            return '{ uint32_t t = %s; %s %s }' % (self.rd(i, a), self.wr(i, a, self.rd(i, b)), self.wr(i, b, 't'))
        if m == 'bswap':
            r = i.reg_name(ops[0].reg)
            return '%s = BSWAP32(%s);' % (r, r)
        if m == 'cdq':
            return 'edx = (uint32_t)((int32_t)eax >> 31);'
        if m == 'cwde':
            return 'eax = (uint32_t)(int32_t)(int16_t)eax;'
        if m == 'cbw':
            return 'eax = (eax & 0xFFFF0000u) | (uint16_t)(int16_t)(int8_t)eax;'
        if m == 'cwd':
            return 'edx = (edx & 0xFFFF0000u) | (uint16_t)(((int16_t)eax) < 0 ? 0xFFFF : 0);'
        if m == 'leave':
            return 'esp = ebp; ebp = RD32(esp); esp += 4;'
        if m == 'push' and ops[0].type == X86_OP_REG and i.reg_name(ops[0].reg) in SEGS:
            return 'esp -= 4; WR32(esp, %s);' % SEGS[i.reg_name(ops[0].reg)]
        if m == 'pop' and ops[0].type == X86_OP_REG and i.reg_name(ops[0].reg) in SEGS:
            return 'esp += 4;'
        if m == 'push':
            v = self.rd(i, ops[0], 4) if ops[0].type != X86_OP_IMM else h(ops[0].imm)
            if ops[0].size == 2 and ops[0].type != X86_OP_IMM:
                return '{ uint32_t t = %s; esp -= 2; WR16(esp, t); }' % v
            return '{ uint32_t t = %s; esp -= 4; WR32(esp, t); }' % v
        if m == 'pop':
            if ops[0].size == 2:
                return '{ uint32_t t = RD16(esp); esp += 2; %s }' % self.wr(i, ops[0], 't')
            if ops[0].type == X86_OP_REG and i.reg_name(ops[0].reg) == 'esp':
                return 'esp = RD32(esp);'
            return '{ uint32_t t = RD32(esp); esp += 4; %s }' % self.wr(i, ops[0], 't')
        if m == 'pushal':
            return ('{ uint32_t t = esp; esp -= 32; WR32(esp+28, eax); WR32(esp+24, ecx); WR32(esp+20, edx); '
                    'WR32(esp+16, ebx); WR32(esp+12, t); WR32(esp+8, ebp); WR32(esp+4, esi); WR32(esp, edi); }')
        if m == 'popal':
            return ('{ edi = RD32(esp); esi = RD32(esp+4); ebp = RD32(esp+8); ebx = RD32(esp+16); '
                    'edx = RD32(esp+20); ecx = RD32(esp+24); eax = RD32(esp+28); esp += 32; }')
        if m in ('pushfd', 'pushf'):
            return 'esp -= 4; WR32(esp, MAKE_EFLAGS());'
        if m in ('popfd', 'popf'):
            return '{ uint32_t t = RD32(esp); esp += 4; SPLIT_EFLAGS(t); }'
        if m == 'lahf':
            return 'eax = (eax & 0xFFFF00FFu) | ((uint32_t)((sf<<7)|(zf<<6)|(af<<4)|(pf<<2)|2|cf) << 8);'
        if m == 'sahf':
            return '{ uint32_t t = eax >> 8; sf = (t>>7)&1; zf = (t>>6)&1; af = (t>>4)&1; pf = (t>>2)&1; cf = t&1; }'
        if m == 'clc':
            return 'cf = 0;'
        if m == 'stc':
            return 'cf = 1;'
        if m == 'cmc':
            return 'cf ^= 1;'
        if m == 'cld':
            return 'c->df = 0;'
        if m == 'std':
            return 'c->df = 1;'
        if m in SETCC:
            return self.wr(i, ops[0], '(%s) ? 1 : 0' % SETCC[m])

        # ---------- arithmetic
        if m in ('add', 'sub', 'cmp', 'and', 'or', 'xor', 'test', 'adc', 'sbb'):
            return self.alu2(i, m, live)
        if m in ('inc', 'dec', 'neg', 'not'):
            return self.alu1(i, m, live)
        if m in SHIFTS:
            return self.shift(i, m, live)
        if m == 'imul':
            return self.imul(i, live)
        if m == 'mul':
            s = ops[0].size
            v = self.rd(i, ops[0])
            if s == 4:
                return ('{ uint64_t r = (uint64_t)eax * (uint32_t)(%s); eax = (uint32_t)r; edx = (uint32_t)(r >> 32); '
                        'cf = of = (edx != 0); }' % v)
            if s == 2:
                return ('{ uint32_t r = (uint32_t)(uint16_t)eax * (uint16_t)(%s); eax = (eax & 0xFFFF0000u) | (uint16_t)r; '
                        'edx = (edx & 0xFFFF0000u) | (uint16_t)(r >> 16); cf = of = ((r >> 16) != 0); }' % v)
            return '{ uint32_t r = (uint32_t)(uint8_t)eax * (uint8_t)(%s); eax = (eax & 0xFFFF0000u) | (uint16_t)r; cf = of = ((r >> 8) != 0); }' % v
        if m in ('div', 'idiv'):
            s = ops[0].size
            v = self.rd(i, ops[0])
            if s == 4:
                if m == 'div':
                    return ('{ uint64_t n = ((uint64_t)edx << 32) | eax; uint32_t d = %s; if (!d) rt_trap(c, %s, "divide by zero"); '
                            'eax = (uint32_t)(n / d); edx = (uint32_t)(n %% d); }' % (v, h(i.address)))
                return ('{ int64_t n = (int64_t)(((uint64_t)edx << 32) | eax); int32_t d = (int32_t)%s; if (!d) rt_trap(c, %s, "divide by zero"); '
                        'eax = (uint32_t)(int32_t)(n / d); edx = (uint32_t)(int32_t)(n %% d); }' % (v, h(i.address)))
            if s == 2:
                if m == 'div':
                    return ('{ uint32_t n = ((edx & 0xFFFFu) << 16) | (eax & 0xFFFFu); uint32_t d = (uint16_t)%s; if (!d) rt_trap(c, %s, "divide by zero"); '
                            'eax = (eax & 0xFFFF0000u) | (uint16_t)(n / d); edx = (edx & 0xFFFF0000u) | (uint16_t)(n %% d); }' % (v, h(i.address)))
                return ('{ int32_t n = (int32_t)(((edx & 0xFFFFu) << 16) | (eax & 0xFFFFu)); int32_t d = (int16_t)%s; if (!d) rt_trap(c, %s, "divide by zero"); '
                        'eax = (eax & 0xFFFF0000u) | (uint16_t)(n / d); edx = (edx & 0xFFFF0000u) | (uint16_t)(n %% d); }' % (v, h(i.address)))
            if m == 'div':
                return ('{ uint32_t n = eax & 0xFFFFu; uint32_t d = (uint8_t)%s; if (!d) rt_trap(c, %s, "divide by zero"); '
                        'eax = (eax & 0xFFFF0000u) | ((n %% d) << 8) | (n / d); }' % (v, h(i.address)))
            return ('{ int32_t n = (int16_t)eax; int32_t d = (int8_t)%s; if (!d) rt_trap(c, %s, "divide by zero"); '
                    'eax = (eax & 0xFFFF0000u) | ((uint32_t)(uint8_t)(n %% d) << 8) | (uint8_t)(n / d); }' % (v, h(i.address)))
        if m in ('bt', 'bts', 'btc', 'btr'):
            a, b = ops
            n = '(%s & %d)' % (self.rd(i, b), BITS[a.size] - 1)
            if a.type == X86_OP_MEM and b.type == X86_OP_REG:
                # bit string addressing: register offset may exceed the operand
                ad = '(%s + (uint32_t)(((int32_t)%s >> 5) * 4))' % (self.addr(i, a), self.rd(i, b))
                n = '(%s & 31)' % self.rd(i, b)
                rd, wr = 'RD32(%s)' % ad, 'WR32(%s, %%s);' % ad
            else:
                rd, wr = self.rd(i, a), None
            s = '{ uint32_t v = %s, n = %s; cf = (v >> n) & 1;' % (rd, n)
            if m != 'bt':
                nv = {'bts': 'v | (1u << n)', 'btr': 'v & ~(1u << n)', 'btc': 'v ^ (1u << n)'}[m]
                s += ' ' + (wr % nv if wr else self.wr(i, a, nv))
            return s + ' }'
        if m in ('bsf', 'bsr'):
            a, b = ops
            f = 'CTZ32(v)' if m == 'bsf' else '(31 - CLZ32(v))'
            return '{ uint32_t v = %s; zf = (v == 0); if (v) { %s } }' % (self.rd(i, b), self.wr(i, a, f))

        # ---------- control flow
        if m == 'jmp':
            t = branch_target(i)
            if t is not None:
                return self.goto_or_tail(fn, t, 0)
            if i.address in self.tables:
                op = ops[0]
                idx = i.reg_name(op.mem.index)
                cases = ' '.join('case %s: goto L_%08X;' % (h(t), t) for t in dict.fromkeys(self.tables[i.address]))
                return 'switch (RD32(%s)) { %s default: %s }' % (self.addr(i, op), cases,
                                                                 self.call_to('RD32(%s)' % self.addr(i, op), 0, tail=True))
            return self.call_to(self.rd(i, ops[0]), 0, tail=True)
        if m in JCC or m in ('jecxz', 'loop', 'loope', 'loopne'):
            t = branch_target(i)
            if m in JCC:
                cond = JCC[m]
                pre = ''
                if (self.prefix == 'nfs4' and i.address == 0x0051A4AF and m == 'je'
                        and t == 0x0051A3BA and self.mod.pe.FILE_HEADER.TimeDateStamp == 0x57334341):
                    cond = '(zf || hle_glide_wide_mode(RD32(ebp - 0x18u), RD32(ebp - 0x14u)))'

            elif m == 'jecxz':
                cond, pre = '(ecx == 0)', ''
            elif m == 'loop':
                cond, pre = '(ecx != 0)', 'ecx--; '
            elif m == 'loope':
                cond, pre = '(ecx != 0 && zf)', 'ecx--; '
            else:
                cond, pre = '(ecx != 0 && !zf)', 'ecx--; '
            return '%sif (%s) { %s }' % (pre, cond, self.goto_or_tail(fn, t, 0))
        if m == 'call':
            post = ' '.join('%sf = c->f_%sf;' % (f.lower(), f.lower()) for f in sorted(live))
            return self.call(fn, i, nxt) + (' ' + post if post else '')
        if m.startswith('ret'):
            n = ops[0].imm if ops else 0
            keep = ''
            if fn.entry in Function.flag_returning:
                keep = 'c->f_cf = cf; c->f_pf = pf; c->f_af = af; c->f_zf = zf; c->f_sf = sf; c->f_of = of; '
            return '%sc->ret_to = RD32(esp); esp += %d; SYNC_OUT; return;' % (keep, 4 + n)
        return self.insn2(fn, i, live, nxt)

    def call(self, fn, i, nxt):
        ops = i.operands
        if True:
            t = branch_target(i)
            if t is not None:
                if t in self.funcs:
                    return self.call_to(None, nxt, direct=self.fname(t))
                if t == nxt:
                    return 'esp -= 4; WR32(esp, %s);' % h(nxt)     # call $+5 (get EIP)
                return self.call_to(h(t), nxt)
            op = ops[0]
            if op.type == X86_OP_MEM and not op.mem.base and not op.mem.index:
                slot = op.mem.disp & 0xFFFFFFFF
                if slot in self.mod.imports:
                    dll, name = self.mod.imports[slot]
                    iid = self.import_ids[(dll, name)]
                    return ('esp -= 4; WR32(esp, %s); SYNC_OUT; RT_API("%s!%s"); %s_imp[%d](c); SYNC_IN; '
                            'if (c->ret_to != %s) rt_bad_return(c, %s);' % (
                                h(nxt), dll, name, self.prefix, iid, h(nxt), h(nxt)))
            return '{ uint32_t t = %s; %s }' % (self.rd(i, op, 4), self.call_to('t', nxt))

    def insn2(self, fn, i, live, nxt):
        m = i.mnemonic
        ops = i.operands
        # ---------- string ops
        if m.endswith(('movsb', 'movsw', 'movsd', 'stosb', 'stosw', 'stosd', 'lodsb', 'lodsw', 'lodsd',
                       'scasb', 'scasw', 'scasd', 'cmpsb', 'cmpsw', 'cmpsd')):
            return self.string(i, m, live)

        # ---------- misc system
        if m == 'cpuid':
            return 'rt_cpuid(c, &eax, &ebx, &ecx, &edx);'
        if m == 'rdtsc':
            return '{ uint64_t t = rt_rdtsc(); eax = (uint32_t)t; edx = (uint32_t)(t >> 32); }'
        if m in ('int3', 'int', 'hlt', 'les', 'lds', 'in', 'out', 'ud2', 'into', 'bound'):
            return 'SYNC_OUT; rt_trap(c, %s, "%s %s"); return;' % (h(i.address), m, i.op_str)
        # NFS4's sound/clear helpers use MMX even with CPUID MMX disabled.
        mmx_ops = {'pxor':0,'por':1,'pand':2,'paddsw':3,'paddw':4,'paddd':5,
                   'pmaddwd':6,'pmulhw':8,'psrlw':9,'psrld':10,'psrlq':11,
                   'psllq':12,'pslld':13,'psrad':14,'punpcklbw':15,
                   'punpcklwd':16,'punpckldq':17,'punpckhbw':18,'punpckhwd':19,
                   'punpckhdq':20,'packuswb':21,'packssdw':22,'packsswb':23}
        if m == 'movd':
            a, b = ops
            ra = i.reg_name(a.reg) if a.type == X86_OP_REG else ''
            rb = i.reg_name(b.reg) if b.type == X86_OP_REG else ''
            if ra.startswith('mm'):
                return 'c->mm[%s] = (uint32_t)(%s);' % (ra[2:], self.rd(i, b, 4))
            if rb.startswith('mm'):
                return self.wr(i, a, '(uint32_t)c->mm[%s]' % rb[2:])
            raise Fail('movd form')
        if m in mmx_ops:
            a, b = ops
            ra = i.reg_name(a.reg) if a.type == X86_OP_REG else ''
            if not ra.startswith('mm'): raise Fail(m + ' non-MMX form')
            if b.type == X86_OP_MEM: value = 'RD64(%s)' % self.addr(i, b)
            elif b.type == X86_OP_IMM: value = h(b.imm)
            elif b.type == X86_OP_REG and i.reg_name(b.reg).startswith('mm'):
                value = 'c->mm[%s]' % i.reg_name(b.reg)[2:]
            else: raise Fail(m + ' source form')
            return 'c->mm[%s] = rt_mmx_binary(%du, c->mm[%s], %s);' % (ra[2:], mmx_ops[m], ra[2:], value)
        if m == 'movq':
            a, b = ops
            ra = i.reg_name(a.reg) if a.type == X86_OP_REG else None
            rb = i.reg_name(b.reg) if b.type == X86_OP_REG else None
            if ra and ra.startswith('mm') and b.type == X86_OP_MEM:
                return 'c->mm[%s] = RD64(%s);' % (ra[2:], self.addr(i, b))
            if rb and rb.startswith('mm') and a.type == X86_OP_MEM:
                return 'WR64(%s, c->mm[%s]);' % (self.addr(i, a), rb[2:])
            if ra and rb and ra.startswith('mm') and rb.startswith('mm'):
                return 'c->mm[%s] = c->mm[%s];' % (ra[2:], rb[2:])
            raise Fail('movq form')

        # ---------- x87
        if m.startswith('f'):
            return self.fpu(i, m)

        raise Fail(m)

    def addr_noseg(self, i, op):
        mem = op.mem
        parts = []
        if mem.base:
            r = i.reg_name(mem.base)
            parts.append('(uint16_t)' + R16[r] if r in R16 else r)
        if mem.index:
            r = i.reg_name(mem.index)
            r = '(uint16_t)' + R16[r] if r in R16 else r
            parts.append('%s*%d' % (r, mem.scale) if mem.scale != 1 else r)
        if mem.disp or not parts:
            parts.append(h(mem.disp))
        return '(uint32_t)(%s)' % (('(uint16_t)(' + '+'.join(parts) + ')') if i.addr_size == 2 else '+'.join(parts))

    def alu2(self, i, m, live):
        a, b = i.operands
        s = a.size
        M, S = MASK[s], SIGN[s]
        A = self.rd(i, a)
        B = self.rd(i, b, s) if b.type == X86_OP_IMM else self.rd(i, b)
        if b.type == X86_OP_IMM:
            B = h(b.imm & {1: 0xFF, 2: 0xFFFF, 4: 0xFFFFFFFF}[s])
        writes = m not in ('cmp', 'test')
        if m in ('xor', 'sub') and a.type == X86_OP_REG and b.type == X86_OP_REG and a.reg == b.reg:
            fl = ' '.join(x for x in ('cf = 0;' if 'C' in live else '', 'of = 0;' if 'O' in live else '',
                                       'zf = 1;' if 'Z' in live else '', 'sf = 0;' if 'S' in live else '',
                                       'pf = 1;' if 'P' in live else '', 'af = 0;' if 'A' in live else '') if x)
            return '%s %s' % (self.wr(i, a, '0'), fl)
        body = []
        if m in ('add',):
            body.append('uint32_t r = (a + b) & %s;' % M)
            if 'C' in live: body.append('cf = (r < a);')
            if 'O' in live: body.append('of = (((a ^ r) & (b ^ r) & %s) != 0);' % S)
        elif m in ('sub', 'cmp'):
            body.append('uint32_t r = (a - b) & %s;' % M)
            if 'C' in live: body.append('cf = (a < b);')
            if 'O' in live: body.append('of = (((a ^ b) & (a ^ r) & %s) != 0);' % S)
        elif m == 'adc':
            body.append('uint64_t t = (uint64_t)a + b + cf; uint32_t r = (uint32_t)t & %s;' % M)
            if 'O' in live: body.append('of = (((a ^ r) & (b ^ r) & %s) != 0);' % S)
            if 'C' in live: body.append('cf = (t > %s);' % M)
        elif m == 'sbb':
            body.append('uint32_t ci = cf; uint32_t r = (a - b - ci) & %s;' % M)
            if 'O' in live: body.append('of = (((a ^ b) & (a ^ r) & %s) != 0);' % S)
            if 'C' in live: body.append('cf = ((uint64_t)a < (uint64_t)b + ci);')
        else:
            op = {'and': '&', 'test': '&', 'or': '|', 'xor': '^'}[m]
            body.append('uint32_t r = a %s b;' % op)
            if 'C' in live: body.append('cf = 0;')
            if 'O' in live: body.append('of = 0;')
        body.append(self.szf(live, s))
        if writes:
            body.append(self.wr(i, a, 'r'))
        return '{ uint32_t a = %s, b = %s; %s }' % (A, B, ' '.join(x for x in body if x))

    def alu1(self, i, m, live):
        a = i.operands[0]
        s = a.size
        M, S = MASK[s], SIGN[s]
        A = self.rd(i, a)
        if m == 'not':
            return self.wr(i, a, '~(%s)' % A)
        body = []
        if m == 'inc':
            body.append('uint32_t r = (a + 1) & %s;' % M)
            if 'O' in live: body.append('of = (r == %s);' % S)
        elif m == 'dec':
            body.append('uint32_t r = (a - 1) & %s;' % M)
            if 'O' in live: body.append('of = (a == %s);' % S)
        else:  # neg
            body.append('uint32_t r = (0u - a) & %s;' % M)
            if 'C' in live: body.append('cf = (a != 0);')
            if 'O' in live: body.append('of = (a == %s);' % S)
        body.append(self.szf(live, s))
        body.append(self.wr(i, a, 'r'))
        return '{ uint32_t a = %s; %s }' % (A, ' '.join(x for x in body if x))

    def shift(self, i, m, live):
        ops = i.operands
        a = ops[0]
        s = a.size
        W = BITS[s]
        M, S = MASK[s], SIGN[s]
        if m in ('shld', 'shrd'):
            b, cnt = ops[1], ops[2]
            n = h(cnt.imm & 31) if cnt.type == X86_OP_IMM else '(ecx & 31)'
            if s != 4:
                raise Fail('16-bit shld')
            if m == 'shld':
                core = 'r = (a << n) | (b >> (32 - n)); cf = (a >> (32 - n)) & 1;'
            else:
                core = 'r = (a >> n) | (b << (32 - n)); cf = (a >> (n - 1)) & 1;'
            return ('{ uint32_t a = %s, b = %s, n = %s, r; if (n) { %s of = ((r ^ a) & 0x80000000u) != 0; %s %s } }' % (
                self.rd(i, a), self.rd(i, b), n, core, self.szf(live, 4), self.wr(i, a, 'r')))
        if len(ops) == 1:
            n = '1u'
        else:
            cnt = ops[1]
            n = h(cnt.imm & 31) if cnt.type == X86_OP_IMM else '(ecx & 31)'
        A = self.rd(i, a)
        if m in ('shl', 'sal'):
            core = ('r = (n < %d) ? ((a << n) & %s) : 0; cf = (n <= %d) ? ((a >> (%d - n)) & 1) : 0; of = ((r & %s) != 0) ^ cf;'
                    % (W, M, W, W, S))
        elif m == 'shr':
            core = 'r = (n < %d) ? (a >> n) : 0; cf = (n <= %d) ? ((a >> (n - 1)) & 1) : 0; of = ((a & %s) != 0);' % (W, W, S)
        elif m == 'sar':
            core = ('int32_t sa = (int32_t)(%s)a; uint32_t k = n < %d ? n : %d; r = (uint32_t)(sa >> k) & %s; '
                    'cf = (uint32_t)(sa >> (k - 1)) & 1; of = 0;' % (ST[s], W, W - 1, M))
        elif m == 'rol':
            core = ('uint32_t k = n %% %d; r = k ? (((a << k) | (a >> (%d - k))) & %s) : a; cf = r & 1; of = ((r & %s) != 0) ^ cf;'
                    % (W, W, M, S))
            return '{ uint32_t a = %s, n = %s, r; if (n) { %s %s } }' % (A, n, core, self.wr(i, a, 'r'))
        elif m == 'ror':
            core = ('uint32_t k = n %% %d; r = k ? (((a >> k) | (a << (%d - k))) & %s) : a; cf = (r & %s) != 0; '
                    'of = (((r << 1) ^ r) & %s) != 0;' % (W, W, M, S, S))
            return '{ uint32_t a = %s, n = %s, r; if (n) { %s %s } }' % (A, n, core, self.wr(i, a, 'r'))
        elif m in ('rcl', 'rcr'):
            fn = 'RCL' if m == 'rcl' else 'RCR'
            return '{ uint32_t a = %s, n = %s, r; if (n) { r = %s%d(a, n, &cf, &of); %s } }' % (
                A, n, fn, W, self.wr(i, a, 'r'))
        return '{ uint32_t a = %s, n = %s, r; if (n) { %s %s %s } }' % (A, n, core, self.szf(live, s), self.wr(i, a, 'r'))

    def imul(self, i, live):
        ops = i.operands
        if len(ops) == 1:
            s = ops[0].size
            v = self.rd(i, ops[0])
            if s == 4:
                return ('{ int64_t r = (int64_t)(int32_t)eax * (int32_t)(%s); eax = (uint32_t)r; edx = (uint32_t)((uint64_t)r >> 32); '
                        'cf = of = (r != (int64_t)(int32_t)r); }' % v)
            if s == 2:
                return ('{ int32_t r = (int32_t)(int16_t)eax * (int16_t)(%s); eax = (eax & 0xFFFF0000u) | (uint16_t)r; '
                        'edx = (edx & 0xFFFF0000u) | (uint16_t)((uint32_t)r >> 16); cf = of = (r != (int16_t)r); }' % v)
            return ('{ int32_t r = (int32_t)(int8_t)eax * (int8_t)(%s); eax = (eax & 0xFFFF0000u) | (uint16_t)r; '
                    'cf = of = (r != (int8_t)r); }' % v)
        if len(ops) == 2:
            a, b = ops[0], ops[1]
            x, y = self.rd(i, a), self.rd(i, b, a.size)
        else:
            a, x, y = ops[0], self.rd(i, ops[1]), self.rd(i, ops[2], ops[0].size)
        s = a.size
        if ops[-1].type == X86_OP_IMM:
            y = h(ops[-1].imm & 0xFFFFFFFF)
        return ('{ int64_t r = (int64_t)(%s)(%s) * (int64_t)(%s)(%s); cf = of = (r != (int64_t)(%s)r); %s }' % (
            ST[s], x, ST[s], y, ST[s], self.wr(i, a, '(uint32_t)r')))

    def string(self, i, m, live):
        rep = None
        parts = m.split()
        base = parts[-1]
        # capstone drops the prefix from the mnemonic for some forms (f2 a5 shows as plain
        # "movsd"), so decide from the prefix bytes. On movs/stos/lods F2 and F3 both mean REP.
        # (it even leaves i.prefix empty there), so read the raw prefix bytes before the opcode.
        pre = bytes(i.bytes[:-1])
        if 0xF3 in pre:
            rep = 'repe' if base[:-1] in ('cmps', 'scas') else 'rep'
        elif 0xF2 in pre:
            rep = 'repne' if base[:-1] in ('cmps', 'scas') else 'rep'
        kind, sz = base[:-1], {'b': 1, 'w': 2, 'd': 4}[base[-1]]
        B = BITS[sz]
        step = '(c->df ? -%d : %d)' % (sz, sz)
        al = {1: '(uint8_t)eax', 2: '(uint16_t)eax', 4: 'eax'}[sz]
        if kind == 'movs':
            one = 'WR%d(edi, RD%d(esi)); esi += d; edi += d;' % (B, B)
        elif kind == 'stos':
            one = 'WR%d(edi, %s); edi += d;' % (B, al)
        elif kind == 'lods':
            one = {1: 'eax = (eax & 0xFFFFFF00u) | RD8(esi);', 2: 'eax = (eax & 0xFFFF0000u) | RD16(esi);',
                   4: 'eax = RD32(esi);'}[sz] + ' esi += d;'
        elif kind in ('scas', 'cmps'):
            if kind == 'scas':
                ab = 'uint32_t a = %s, b = RD%d(edi); edi += d;' % (al, B)
            else:
                ab = 'uint32_t a = RD%d(esi), b = RD%d(edi); esi += d; edi += d;' % (B, B)
            fl = 'uint32_t r = (a - b) & %s; cf = (a < b); of = (((a ^ b) & (a ^ r) & %s) != 0); %s' % (
                MASK[sz], SIGN[sz], self.szf(ALL_FLAGS, sz))
            one = '{ %s %s }' % (ab, fl)
            if rep:
                stop = 'if (!zf) break;' if rep in ('repe', 'repz', 'rep') else 'if (zf) break;'
                return '{ int32_t d = %s; while (ecx) { ecx--; %s %s } }' % (step, one, stop)
            return '{ int32_t d = %s; %s }' % (step, one)
        else:
            raise Fail('string ' + m)
        if rep:
            if kind == 'movs' and sz == 4:
                return '{ int32_t d = %s; if (d > 0) { rt_movsd(&esi, &edi, ecx); ecx = 0; } else while (ecx) { %s ecx--; } }' % (step, one)
            if kind == 'movs' and sz == 1:
                return '{ int32_t d = %s; if (d > 0) { rt_movsb(&esi, &edi, ecx); ecx = 0; } else while (ecx) { %s ecx--; } }' % (step, one)
            if kind == 'stos' and sz == 4:
                return '{ int32_t d = %s; if (d > 0) { rt_stosd(&edi, eax, ecx); ecx = 0; } else while (ecx) { %s ecx--; } }' % (step, one)
            return '{ int32_t d = %s; while (ecx) { %s ecx--; } }' % (step, one)
        return '{ int32_t d = %s; %s }' % (step, one)

    # ---------- x87
    def fpu(self, i, m):
        ops = i.operands
        A = lambda op: self.addr(i, op)
        def sti(op):
            return int(i.reg_name(op.reg)[3])
        ldm = {4: 'RDF32(%s)', 8: 'RDF64(%s)', 10: 'RDF80(%s)'}
        stm = {4: 'WRF32(%s, %s);', 8: 'WRF64(%s, %s);', 10: 'WRF80(%s, %s);'}
        if m == 'fld':
            op = ops[0]
            if op.type == X86_OP_MEM:
                return 'FPUSH(%s);' % (ldm[op.size] % A(op))
            return ('{ int k = STSLOT(%d); double t = c->st[k]; int64_t q = c->ist[k]; uint8_t v = c->iv[k]; '
                    'FPUSH(t); c->ist[c->top] = q; c->iv[c->top] = v; }' % sti(op))
        if m in ('fst', 'fstp'):
            op = ops[0]
            pop = ' FPOP();' if m == 'fstp' else ''
            if op.type == X86_OP_MEM:
                return (stm[op.size] % (A(op), 'ST(0)')) + pop
            return 'ST(%d) = ST(0); FCOPYTAG(%d, 0);%s' % (sti(op), sti(op), pop)
        if m == 'fild':
            op = ops[0]
            if op.size == 8:
                return 'FILD64(%s);' % A(op)
            t = {2: '(double)(int16_t)RD16(%s)', 4: '(double)(int32_t)RD32(%s)'}[op.size]
            return 'FPUSH(%s);' % (t % A(op))
        if m in ('fist', 'fistp'):
            op = ops[0]
            pop = ' FPOP();' if m == 'fistp' else ''
            t = {2: 'WR16(%s, (uint16_t)fpu_toi16(c, ST(0)));', 4: 'WR32(%s, (uint32_t)fpu_toi32(c, ST(0)));',
                 8: 'WR64(%s, (uint64_t)fpu_toi64_st0(c));'}[op.size]
            return (t % A(op)) + pop
        consts = {'fldz': '0.0', 'fld1': '1.0', 'fldpi': 'M_PI_X87', 'fldl2e': 'M_LOG2E_X87',
                  'fldln2': 'M_LN2_X87', 'fldl2t': 'M_LOG2T_X87', 'fldlg2': 'M_LOG10_2_X87'}
        if m in consts:
            return 'FPUSH(%s);' % consts[m]
        if m == 'fxch':
            k = sti(ops[-1]) if ops else 1
            return 'FXCH(c, %d);' % k
        unary = {'fchs': 'ST(0) = -ST(0);', 'fabs': 'ST(0) = fabs(ST(0));', 'fsqrt': 'ST(0) = sqrt(ST(0));',
                 'fsin': 'ST(0) = sin(ST(0)); c->fsw &= ~0x0400;', 'fcos': 'ST(0) = cos(ST(0)); c->fsw &= ~0x0400;',
                 'frndint': 'ST(0) = fpu_round(c, ST(0));', 'f2xm1': 'ST(0) = exp2(ST(0)) - 1.0;',
                 'fptan': 'ST(0) = tan(ST(0)); FPUSH(1.0); c->fsw &= ~0x0400;',
                 'fsincos': '{ double v = ST(0); ST(0) = sin(v); c->iv[c->top] = 0; FPUSH(cos(v)); c->fsw &= ~0x0400; }',
                 'fpatan': '{ double t = atan2(ST(1), ST(0)); FPOP(); ST(0) = t; }',
                 'fyl2x': '{ double t = ST(1) * log2(ST(0)); FPOP(); ST(0) = t; }',
                 'fscale': 'ST(0) = ldexp(ST(0), (int)trunc(ST(1)));',
                 'fprem': 'ST(0) = fmod(ST(0), ST(1)); c->fsw &= ~0x0400;',
                 'fninit': 'fpu_init(c);', 'finit': 'fpu_init(c);', 'fnclex': 'c->fsw &= 0x7F00;',
                 'ftst': 'fpu_compare(c, ST(0), 0.0);'}
        if m in unary:
            return unary[m]
        if m in ('fnstsw', 'fstsw'):
            op = ops[0]
            if op.type == X86_OP_REG:
                return 'eax = (eax & 0xFFFF0000u) | FPU_SW();'
            return 'WR16(%s, FPU_SW());' % A(op)
        if m in ('fnstcw', 'fstcw'):
            return 'WR16(%s, c->fcw);' % A(ops[0])
        if m == 'fldcw':
            return 'c->fcw = RD16(%s);' % A(ops[0])
        if m in ('fnsave', 'fsave'):
            return 'fpu_save(c, %s);' % A(ops[0])
        if m == 'frstor':
            return 'fpu_restore(c, %s);' % A(ops[0])
        if m in ('fcom', 'fcomp', 'fcompp', 'fucom', 'fucomp', 'fucompp', 'ficom', 'ficomp'):
            if ops and ops[0].type == X86_OP_MEM:
                op = ops[0]
                if m.startswith('fi'):
                    src = {2: '(double)(int16_t)RD16(%s)', 4: '(double)(int32_t)RD32(%s)'}[op.size] % A(op)
                else:
                    src = ldm[op.size] % A(op)
            else:
                src = 'ST(%d)' % (sti(ops[0]) if ops else 1)
            pops = 2 if m.endswith('pp') else 1 if m.endswith('p') else 0
            return 'fpu_compare(c, ST(0), %s);%s' % (src, ' FPOP();' * pops)
        ar = {'fadd': '+', 'fmul': '*', 'fsub': '-', 'fsubr': '-', 'fdiv': '/', 'fdivr': '/'}
        base = m[:-1] if m.endswith('p') and m[:-1] in ar else m
        ibase = base[0] + base[2:] if base.startswith('fi') and ('f' + base[2:]) in ar else None
        if base.startswith('fi') and ('f' + base[2:]) in ar:
            op = ops[0]
            src = {2: '(double)(int16_t)RD16(%s)', 4: '(double)(int32_t)RD32(%s)'}[op.size] % A(op)
            o = ar['f' + base[2:]]
            rev = base.endswith('r')
            return 'ST(0) = %s;' % (('%s %s ST(0)' % (src, o)) if rev else ('ST(0) %s %s' % (o, src)))
        if base in ar:
            o = ar[base]
            rev = base.endswith('r')
            pop = m.endswith('p')
            if ops and ops[0].type == X86_OP_MEM:
                src = ldm[ops[0].size] % A(ops[0])
                dst = 'ST(0)'
            elif pop:
                k = sti(ops[0]) if ops else 1
                dst, src = 'ST(%d)' % k, 'ST(0)'
            elif len(ops) == 2:
                dst, src = 'ST(%d)' % sti(ops[0]), 'ST(%d)' % sti(ops[1])
            elif len(ops) == 1:
                dst, src = 'ST(0)', 'ST(%d)' % sti(ops[0])
            else:
                dst, src = 'ST(1)', 'ST(0)'
            expr = ('%s %s %s' % (src, o, dst)) if rev else ('%s %s %s' % (dst, o, src))
            return '%s = %s;%s' % (dst, expr, ' FPOP();' if pop else '')
        if m == 'ffree':
            return ';'
        raise Fail(m)


# ---------------------------------------------------------------- driver

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('pe')
    ap.add_argument('outdir')
    ap.add_argument('prefix')
    ap.add_argument('--base', type=lambda x: int(x, 0))
    ap.add_argument('--seeds')
    ap.add_argument('--per-file', type=int, default=150)
    args = ap.parse_args()

    mod = Module(args.pe, args.base)
    extra = []
    if args.seeds and os.path.exists(args.seeds):
        for line in open(args.seeds):
            line = line.split('#')[0].strip()
            if line:
                extra.append(int(line, 16))
    funcs, tables = discover(mod, extra)
    funcs = sorted(funcs)
    print('%s: %d functions, %d jump tables' % (args.prefix, len(funcs), len(tables)))

    # functions whose callers consume the flags they return (Watcom asm helpers)
    fset = set(funcs)
    flag_ret = set()
    for f in funcs:
        fn = Function(mod, f, fset, tables)
        fn.liveness()
        for a in fn.order:
            i = fn.insns[a]
            if i is not None and i.mnemonic == 'call' and fn.live_out.get(a):
                t = branch_target(i)
                if t is not None:
                    flag_ret.add(t)
    work = list(flag_ret)
    while work:
        f = work.pop()
        if f not in fset:
            continue
        fn = Function(mod, f, fset, tables)
        for a in fn.order:
            i = fn.insns[a]
            if i is not None and i.mnemonic == 'jmp':
                t = branch_target(i)
                if t is not None and t in fset and t != f and t not in flag_ret:
                    flag_ret.add(t)
                    work.append(t)
    Function.flag_returning = flag_ret
    print('%s: %d flag-returning functions: %s' % (args.prefix, len(flag_ret), ' '.join('%08X' % x for x in sorted(flag_ret))))

    import_ids = {}
    for slot, key in sorted(mod.imports.items()):
        import_ids.setdefault(key, len(import_ids))

    os.makedirs(args.outdir, exist_ok=True)
    em = Emitter(mod, args.prefix, set(funcs), tables, import_ids)
    total_insns = 0
    files = []
    for k in range(0, len(funcs), args.per_file):
        em.lines = ['#include "rt.h"', '#include "%s_decl.h"' % args.prefix, '']
        for f in funcs[k:k + args.per_file]:
            fn = Function(mod, f, em.funcs, tables)
            total_insns += len(fn.insns)
            em.function(fn)
        name = '%s_%03d.c' % (args.prefix, k // args.per_file)
        output = os.path.join(args.outdir, name)
        content = '\n'.join(em.lines)
        if not os.path.exists(output) or open(output).read() != content:
            open(output, 'w').write(content)
        files.append(name)

    # declarations + function table + image description
    d = ['#pragma once', '#include "rt.h"']
    d.append('extern HleFn %s_imp[%d];' % (args.prefix, max(1, len(import_ids))))
    d += ['void %s(Cpu *c);' % em.fname(f) for f in funcs]
    open(os.path.join(args.outdir, '%s_decl.h' % args.prefix), 'w').write('\n'.join(d) + '\n')

    t = ['#include "rt.h"', '#include "%s_decl.h"' % args.prefix, '']
    t.append('HleFn %s_imp[%d];' % (args.prefix, max(1, len(import_ids))))
    t.append('const uint32_t %s_imp_count = %d;' % (args.prefix, len(import_ids)))
    t.append('const GuestFn %s_functions[] = {' % args.prefix)
    t += ['  { %s, %s },' % (h(f), em.fname(f)) for f in funcs]
    t.append('};')
    t.append('const uint32_t %s_function_count = %d;' % (args.prefix, len(funcs)))
    t.append('const HleImport %s_imports[] = {' % args.prefix)
    for slot, (dll, name) in sorted(mod.imports.items()):
        t.append('  { %s, %d, "%s", "%s" },' % (h(slot), import_ids[(dll, name)], dll, name))
    t.append('};')
    t.append('const uint32_t %s_import_count = %d;' % (args.prefix, len(mod.imports)))
    t.append('const HleExport %s_exports[] = {' % args.prefix)
    for a, name in sorted(mod.exports.items()):
        t.append('  { %s, "%s" },' % (h(a), name))
    t.append('  { 0, 0 }')
    t.append('};')
    open(os.path.join(args.outdir, '%s_tables.c' % args.prefix), 'w').write('\n'.join(t) + '\n')

    # raw image for the loader
    open(os.path.join(args.outdir, '%s.img' % args.prefix), 'wb').write(bytes(mod.img))
    info = {'base': mod.base, 'size': len(mod.img), 'entry': mod.base + mod.pe.OPTIONAL_HEADER.AddressOfEntryPoint}
    P = args.prefix.upper()
    open(os.path.join(args.outdir, '%s_image.h' % args.prefix), 'w').write(
        '#define %s_BASE 0x%08Xu\n#define %s_SIZE 0x%08Xu\n#define %s_ENTRY 0x%08Xu\n#define %s_TIMESTAMP 0x%08Xu\n' % (
            P, info['base'], P, info['size'], P, info['entry'], P, mod.pe.FILE_HEADER.TimeDateStamp))
    print('%s: %d instructions emitted into %d files; unsupported: %s' % (
        args.prefix, total_insns, len(files), dict(em.unsupported.most_common(20))))


if __name__ == '__main__':
    main()
