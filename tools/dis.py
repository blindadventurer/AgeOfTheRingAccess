"""Disassemble game.dat (32-bit) around an address, annotating string operands.

Usage: python -P dis.py <exe> <va-hex> [count] [back-bytes]
"""
import re
import struct
import sys

import capstone
import pefile

pe = pefile.PE(sys.argv[1], fast_load=True)
data = pe.__data__
base = pe.OPTIONAL_HEADER.ImageBase


def va_to_off(va):
    rva = va - base
    for s in pe.sections:
        if s.VirtualAddress <= rva < s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData):
            o = s.PointerToRawData + rva - s.VirtualAddress
            return o if o < s.PointerToRawData + s.SizeOfRawData else None
    return None


def string_at(va):
    o = va_to_off(va)
    if o is None:
        return None
    m = re.match(rb'[\x20-\x7e]{3,120}\0', data[o:o + 121])
    if m:
        return m.group()[:-1].decode()
    m = re.match(rb'(?:[\x20-\x7e]\0){3,80}\0\0', data[o:o + 162])
    if m:
        return 'L' + m.group()[:-2].decode('utf-16-le')
    return None


def main():
    va = int(sys.argv[2], 16)
    count = int(sys.argv[3]) if len(sys.argv) > 3 else 40
    back = int(sys.argv[4]) if len(sys.argv) > 4 else 0
    start = va - back
    o = va_to_off(start)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    n = 0
    for ins in md.disasm(data[o:o + count * 16], start):
        note = ''
        for imm in re.findall(r'0x[0-9a-f]{6,8}', ins.op_str):
            s = string_at(int(imm, 16))
            if s:
                note += f'  ; "{s}"'
        mark = '>>' if ins.address == va else '  '
        print(f'{mark}{ins.address:08x}  {ins.mnemonic:7s} {ins.op_str}{note}')
        n += 1
        if n >= count:
            break


if __name__ == '__main__':
    main()
