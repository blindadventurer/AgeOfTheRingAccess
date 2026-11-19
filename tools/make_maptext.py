"""Collect the texts of every mission's map.str into one file the mod reads (maptext.txt).

Usage: python -I make_maptext.py <aotr folder> <out file>

Each map folder may carry a map.str (same format as lotr.str: label line, "text" line, END)
with the texts its scripts show: hints, captions, dialogue subtitles ("SCRIPT:mgfotr_gandalf316").
The mod does not know which map is loaded, so all of them go into one table; 11 of 3475 labels
differ between maps (checked 2026-11-01), the first folder in name order wins and the others
are listed on stderr. Output: UTF-8, one "LABEL<TAB>text" per line, label upper-case,
"\\n" in a text turned into ". ".
"""
import os
import re
import sys

ENTRY = re.compile(r'^([^\s"/;][^\r\n]*?)\s*\r?\n\s*"(.*?)"\s*\r?\n\s*END', re.M | re.S | re.I)


def read_str(path):
    raw = open(path, 'rb').read()
    try:
        text = raw.decode('cp1252')
    except UnicodeDecodeError:
        text = raw.decode('latin-1')
    out = {}
    for m in ENTRY.finditer(text):
        t = m.group(2).replace('\\n', '. ').replace('\r', ' ').replace('\n', ' ').replace('\t', ' ')
        t = re.sub(r'\s+', ' ', t).strip()
        if t:
            out[m.group(1).strip().upper()] = t
    return out


def main():
    aotr, out_path = sys.argv[1], sys.argv[2]
    maps = os.path.join(aotr, 'maps')
    table, origin, files = {}, {}, 0
    for folder in sorted(os.listdir(maps)):
        path = os.path.join(maps, folder, 'map.str')
        if not os.path.exists(path):
            continue
        files += 1
        for k, v in read_str(path).items():
            if k not in table:
                table[k], origin[k] = v, folder
            elif table[k] != v:
                print(f'conflict {k}: kept {origin[k]}, ignored {folder}', file=sys.stderr)
    with open(out_path, 'w', encoding='utf-8', newline='\n') as f:
        for k in sorted(table):
            f.write(f'{k}\t{table[k]}\n')
    print(f'{files} map.str files, {len(table)} labels -> {out_path}')


if __name__ == '__main__':
    main()
