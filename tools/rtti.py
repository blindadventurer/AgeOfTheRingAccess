"""Print the MSVC RTTI class name for vtable addresses (32-bit).
Usage: python -P rtti.py <exe> <vtable-hex>...
"""
import struct, sys, re
import pefile
pe = pefile.PE(sys.argv[1], fast_load=True); d = pe.__data__; base = pe.OPTIONAL_HEADER.ImageBase
def off(va):
    rva = va - base
    for s in pe.sections:
        if s.VirtualAddress <= rva < s.VirtualAddress + s.SizeOfRawData:
            return s.PointerToRawData + rva - s.VirtualAddress
def u32(va):
    o = off(va); return struct.unpack_from('<I', d, o)[0] if o is not None else None
for a in sys.argv[2:]:
    vt = int(a, 16); col = u32(vt - 4)
    td = u32(col + 12) if col else None
    o = off(td + 8) if td else None
    name = re.match(rb'[ -~]+', d[o:o + 200]).group().decode() if o is not None else None
    print(hex(vt), 'COL', hex(col) if col else None, name)
