"""Recursive-descent disassembly survey of a PE: reachable code, functions,
instruction mnemonics and indirect control flow. Seeds: entry point, exports
and every relocated pointer that lands in the code section."""
import collections, sys
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32, CS_GRP_JUMP, CS_GRP_CALL, CS_GRP_RET
from capstone.x86 import X86_OP_IMM, X86_OP_MEM

path = sys.argv[1]
pe = pefile.PE(path)
base = pe.OPTIONAL_HEADER.ImageBase
img = pe.get_memory_mapped_image()
code = next(s for s in pe.sections if s.Characteristics & 0x20)
c0, c1 = base + code.VirtualAddress, base + code.VirtualAddress + code.SizeOfRawData

md = Cs(CS_ARCH_X86, CS_MODE_32)
md.detail = True

# relocated dwords pointing into code = function pointers, jump tables, vtables
reloc_targets = set()
reloc_sites = set()
if hasattr(pe, 'DIRECTORY_ENTRY_BASERELOC'):
    for blk in pe.DIRECTORY_ENTRY_BASERELOC:
        for e in blk.entries:
            if e.type == 3:
                site = base + e.rva
                reloc_sites.add(site)
                v = int.from_bytes(img[e.rva:e.rva + 4], 'little')
                if c0 <= v < c1:
                    reloc_targets.add((site, v))

seeds = {base + pe.OPTIONAL_HEADER.AddressOfEntryPoint}
if hasattr(pe, 'DIRECTORY_ENTRY_EXPORT'):
    seeds |= {base + e.address for e in pe.DIRECTORY_ENTRY_EXPORT.symbols}
data_ptrs = {v for site, v in reloc_targets if not (c0 <= site < c1)}
code_ptrs = {v for site, v in reloc_targets if c0 <= site < c1}
seeds |= data_ptrs

insns = {}
funcs = set(seeds)
mn = collections.Counter()
indirect_jmp = []
indirect_call = 0
work = list(seeds)
while work:
    a = work.pop()
    while c0 <= a < c1 and a not in insns:
        off = a - base
        try:
            i = next(md.disasm(bytes(img[off:off + 16]), a))
        except StopIteration:
            print('bad decode at %08x' % a)
            break
        insns[a] = i
        mn[i.mnemonic] += 1
        nxt = a + i.size
        if i.group(CS_GRP_RET) or i.mnemonic == 'hlt':
            break
        if i.group(CS_GRP_CALL):
            op = i.operands[0]
            if op.type == X86_OP_IMM:
                funcs.add(op.imm); work.append(op.imm)
            else:
                indirect_call += 1
        elif i.group(CS_GRP_JUMP):
            op = i.operands[0]
            if op.type == X86_OP_IMM:
                work.append(op.imm)
                if i.mnemonic == 'jmp':
                    break
            else:
                indirect_jmp.append(i)
                # jump table: jmp [reg*4 + table]
                if op.type == X86_OP_MEM and op.mem.disp and op.mem.index:
                    t = op.mem.disp
                    while t in reloc_sites:
                        v = int.from_bytes(img[t - base:t - base + 4], 'little')
                        if not (c0 <= v < c1):
                            break
                        work.append(v)
                        t += 4
                break
        a = nxt

covered = sum(i.size for i in insns.values())
print('code section %08x-%08x (%d bytes)' % (c0, c1, c1 - c0))
print('reachable instructions %d, bytes %d (%.0f%% of section)' % (len(insns), covered, 100 * covered / (c1 - c0)))
print('function entries %d, pointer seeds from data %d, code-embedded pointers %d' % (len(funcs), len(data_ptrs), len(code_ptrs)))
print('indirect calls %d, indirect jumps %d (table-form %d)' % (
    indirect_call, len(indirect_jmp), sum(1 for i in indirect_jmp if i.operands[0].type == X86_OP_MEM and i.operands[0].mem.index)))
print('distinct mnemonics %d' % len(mn))
print(' '.join('%s:%d' % kv for kv in mn.most_common()))
