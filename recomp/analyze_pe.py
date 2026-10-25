"""Dump PE layout, imports and exports of the NFS3 binaries."""
import math, sys, collections
import pefile


def entropy(b):
    if not b:
        return 0
    c = collections.Counter(b)
    return -sum(v / len(b) * math.log2(v / len(b)) for v in c.values())


for path in sys.argv[1:]:
    pe = pefile.PE(path)
    oh = pe.OPTIONAL_HEADER
    print('=' * 70)
    print(path)
    print('base %08x entry %08x subsystem %d linker %d.%d timestamp %x' % (
        oh.ImageBase, oh.AddressOfEntryPoint, oh.Subsystem, oh.MajorLinkerVersion, oh.MinorLinkerVersion,
        pe.FILE_HEADER.TimeDateStamp))
    print('relocs:', bool(oh.DATA_DIRECTORY[5].Size), 'tls:', bool(oh.DATA_DIRECTORY[9].Size))
    for s in pe.sections:
        print('  %-8s va %08x vsz %08x raw %08x flags %08x entropy %.2f' % (
            s.Name.rstrip(b'\0').decode(), s.VirtualAddress, s.Misc_VirtualSize, s.SizeOfRawData,
            s.Characteristics, entropy(s.get_data())))
    if hasattr(pe, 'DIRECTORY_ENTRY_IMPORT'):
        for imp in pe.DIRECTORY_ENTRY_IMPORT:
            names = [(i.name or b'#%d' % i.ordinal).decode() for i in imp.imports]
            print('  import %s (%d): %s' % (imp.dll.decode(), len(names), ' '.join(names)))
    if hasattr(pe, 'DIRECTORY_ENTRY_EXPORT'):
        names = [(e.name or b'#%d' % e.ordinal).decode() for e in pe.DIRECTORY_ENTRY_EXPORT.symbols]
        print('  exports (%d): %s' % (len(names), ' '.join(names)))
