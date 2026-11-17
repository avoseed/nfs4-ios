# Name the host function at a crash: py -3 tools/host_symbol.py <module offset hex> [exe]
#   the offset is the "module +XXXX" of a "FATAL: host exception" log line; exe defaults to the symbol build
#   (recomp/build_host.py --sym -> recomp/build/nfs4host_sym.exe, run it with NFS4_HOST_EXE). Uses Windows dbghelp.
import ctypes, ctypes.wintypes as wt, sys, pathlib

off = int(sys.argv[1], 16)
exe = sys.argv[2] if len(sys.argv) > 2 else str(pathlib.Path(__file__).resolve().parents[1] / 'recomp/build/nfs4host_sym.exe')
dbg = ctypes.WinDLL('dbghelp')
proc = wt.HANDLE(0x1234)
dbg.SymSetOptions(0x2 | 0x10)                      # undecorated names, line numbers
assert dbg.SymInitialize(proc, str(pathlib.Path(exe).parent).encode(), False)
dbg.SymLoadModuleEx.restype = ctypes.c_uint64
base = 0x140000000
assert dbg.SymLoadModuleEx(proc, None, exe.encode(), None, ctypes.c_uint64(base), 0, None, 0), 'no symbols'

class SYMBOL_INFO(ctypes.Structure):
    _fields_ = [('SizeOfStruct', wt.ULONG), ('TypeIndex', wt.ULONG), ('Reserved', ctypes.c_uint64 * 2), ('Index', wt.ULONG),
                ('Size', wt.ULONG), ('ModBase', ctypes.c_uint64), ('Flags', wt.ULONG), ('Value', ctypes.c_uint64),
                ('Address', ctypes.c_uint64), ('Register', wt.ULONG), ('Scope', wt.ULONG), ('Tag', wt.ULONG),
                ('NameLen', wt.ULONG), ('MaxNameLen', wt.ULONG), ('Name', ctypes.c_char * 512)]

class LINE(ctypes.Structure):
    _fields_ = [('SizeOfStruct', wt.DWORD), ('Key', ctypes.c_void_p), ('LineNumber', wt.DWORD), ('FileName', ctypes.c_char_p),
                ('Address', ctypes.c_uint64)]

s = SYMBOL_INFO(); s.SizeOfStruct = 88; s.MaxNameLen = 511
disp = ctypes.c_uint64()
if dbg.SymFromAddr(proc, ctypes.c_uint64(base + off), ctypes.byref(disp), ctypes.byref(s)):
    print('%s+0x%x' % (s.Name.decode(), disp.value))
ln = LINE(); ln.SizeOfStruct = ctypes.sizeof(LINE); d32 = wt.DWORD()
if dbg.SymGetLineFromAddr64(proc, ctypes.c_uint64(base + off), ctypes.byref(d32), ctypes.byref(ln)):
    print('%s:%d' % (ln.FileName.decode(), ln.LineNumber))
