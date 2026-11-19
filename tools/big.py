"""List or extract SAGE .big archives (BIGF / BIG4).

Usage:
  python -I big.py list <file.big> [filter]
  python -I big.py extract <file.big> <out_dir> [filter]
Format: magic(4) archive_size(LE u32) count(BE u32) header_size(BE u32),
then per entry offset(BE u32) size(BE u32) name(zero-terminated).
"""
import os
import struct
import sys


def entries(path):
    with open(path, 'rb') as f:
        head = f.read(16)
        if head[:4] not in (b'BIGF', b'BIG4'):
            raise SystemExit(f'{path}: not a BIG archive')
        count, = struct.unpack('>I', head[8:12])
        out = []
        for _ in range(count):
            off, size = struct.unpack('>II', f.read(8))
            name = bytearray()
            while (c := f.read(1)) not in (b'\0', b''):
                name += c
            out.append((name.decode('latin-1'), off, size))
        return out


def main():
    cmd, path = sys.argv[1], sys.argv[2]
    if cmd == 'list':
        flt = sys.argv[3].lower() if len(sys.argv) > 3 else ''
        for name, off, size in entries(path):
            if flt in name.lower():
                print(f'{size:10d}  {name}')
    elif cmd == 'extract':
        out_dir = sys.argv[3]
        flt = sys.argv[4].lower() if len(sys.argv) > 4 else ''
        with open(path, 'rb') as f:
            for name, off, size in entries(path):
                if flt not in name.lower():
                    continue
                f.seek(off)
                dest = os.path.join(out_dir, name.replace('\\', os.sep))
                os.makedirs(os.path.dirname(dest), exist_ok=True)
                with open(dest, 'wb') as o:
                    o.write(f.read(size))
                print(dest)


if __name__ == '__main__':
    main()
