"""List or extract entries of a PyInstaller onefile executable (CArchive), read only.

Usage: python -I pyi_list.py <exe> [out_dir [name-filter]]
Nothing from the archive is executed: entries are only decompressed to files.
Layout (PyInstaller >= 4): cookie "MEI\\x0c\\x0b\\x0a\\x0b\\x0e" + package length, TOC offset,
TOC length, python version, python library name; TOC entries are
!iIIIBc + name, offsets relative to the package start.
"""
import os
import struct
import sys
import zlib

MAGIC = b'MEI\x0c\x0b\x0a\x0b\x0e'


def toc(path):
    data = open(path, 'rb').read()
    cookie = data.rfind(MAGIC)
    if cookie < 0:
        raise SystemExit('not a PyInstaller archive')
    pkg_len, toc_off, toc_len, pyver = struct.unpack('!IIII', data[cookie + 8:cookie + 24])
    start = cookie + 24 + 64 - pkg_len  # the cookie ends the package
    entries = []
    pos = start + toc_off
    end = pos + toc_len
    while pos < end:
        size, off, csize, usize, flag, typ = struct.unpack('!iIIIBc', data[pos:pos + 18])
        name = data[pos + 18:pos + size].rstrip(b'\0').decode('utf-8', 'replace')
        entries.append((name, start + off, csize, usize, flag, typ.decode()))
        pos += size
    return data, pyver, entries


def main():
    data, pyver, entries = toc(sys.argv[1])
    out = sys.argv[2] if len(sys.argv) > 2 else None
    flt = sys.argv[3].lower() if len(sys.argv) > 3 else ''
    print('python', pyver, 'entries', len(entries))
    for name, off, csize, usize, flag, typ in entries:
        if flt and flt not in name.lower():
            continue
        print(f'{typ} {usize:10d} {name}')
        if out:
            blob = data[off:off + csize]
            if flag:
                blob = zlib.decompress(blob)
            dest = os.path.join(out, name.replace('/', os.sep).replace('\\', os.sep))
            os.makedirs(os.path.dirname(dest) or out, exist_ok=True)
            with open(dest + ('.pyc_body' if typ in 's' else ''), 'wb') as f:
                f.write(blob)


if __name__ == '__main__':
    main()
