"""Audit every campaign mission for things a blind player cannot find with the scanner.

Usage: python -I missions_audit.py <aotr folder> <out.txt>

For each map folder whose map.ini has a MissionObjectiveList:
  - objective texts (map.str, then data\\lotr.str), flagged by verbs that need finding
    a place or an object (find, collect, search, extinguish, stand, reach...);
  - templates the mission scripts name (exact script strings that are object templates)
    or that are placed with a script name, which the scanner does not list: not
    SELECTABLE, or side Civilian. Those are the candidates for labels.ini;
  - named trigger areas (places scripts test for).
Template data comes from data\\ini\\object and the map's own map.ini.
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mapfile  # noqa: E402

VERBS = re.compile(r'\b(find|collect|gather|search|locate|retrieve|pick up|rescue|free|extinguish|fire|burn|'
                   r'stand|answer|reach|go to|travel|escort|bring|deliver|light|ring|beacon|talk|speak|'
                   r'investigate|explore|hide|sneak|use)\b', re.I)


def read_str(path):
    out = {}
    if not os.path.exists(path):
        return out
    text = open(path, 'rb').read().decode('latin-1')
    for m in re.finditer(r'^([^\s"/][^\r\n]*?)\s*\r?\n\s*"(.*?)"\s*\r?\n\s*END', text, re.M | re.S | re.I):
        out[m.group(1).strip().upper()] = m.group(2).replace('\\n', ' ').strip()
    return out


def read_objects_ini(paths):
    """template (upper) -> {kindof, side, display}"""
    db = {}
    for path in paths:
        try:
            text = open(path, 'rb').read().decode('latin-1')
        except OSError:
            continue
        cur = None
        for line in text.splitlines():
            line = line.split(';')[0].strip()
            if not line:
                continue
            m = re.match(r'(Object|ChildObject|ObjectReskin)\s+(\S+)(?:\s+(\S+))?', line)
            if m:
                cur = m.group(2).upper()
                d = db.setdefault(cur, {})
                if m.group(1) != 'Object' and m.group(3):
                    d.setdefault('parent', m.group(3).upper())
                continue
            if cur is None:
                continue
            m = re.match(r'(KindOf|Side|DisplayName)\s*=\s*(.*)', line, re.I)
            if m:
                key = m.group(1).lower()
                if key not in db[cur]:
                    db[cur][key] = m.group(2).strip()
    return db


def resolved(db, name, key, depth=0):
    d = db.get(name.upper())
    if not d or depth > 8:
        return ''
    if key in d:
        return d[key]
    return resolved(db, d['parent'], key, depth + 1) if 'parent' in d else ''


def walk_ini(folder):
    for root, _, files in os.walk(folder):
        for f in files:
            if f.lower().endswith('.ini'):
                yield os.path.join(root, f)


def main():
    aotr, out_path = sys.argv[1], sys.argv[2]
    lotr = read_str(os.path.join(aotr, 'data', 'lotr.str'))
    base_db = read_objects_ini(walk_ini(os.path.join(aotr, 'data', 'ini', 'object')))
    maps = os.path.join(aotr, 'maps')
    out = open(out_path, 'w', encoding='utf-8')
    summary = []
    for folder in sorted(os.listdir(maps)):
        mdir = os.path.join(maps, folder)
        ini = os.path.join(mdir, 'map.ini')
        mpath = os.path.join(mdir, folder + '.map')
        if not os.path.exists(ini) or not os.path.exists(mpath):
            continue
        ini_text = open(ini, 'rb').read().decode('latin-1')
        if 'MissionObjectiveList' not in ini_text:
            continue
        strs = dict(lotr)
        strs.update(read_str(os.path.join(mdir, 'map.str')))
        db = dict(base_db)
        db.update(read_objects_ini([ini]))
        tags = re.findall(r'(Bonus)?MissionObjectiveTag\s*=\s*(\S+)', ini_text)
        try:
            raw, pos, names = mapfile.load(mpath)
        except Exception as e:  # noqa: BLE001
            out.write(f'\n=== {folder}: map unreadable ({e})\n')
            continue
        script = [s for _, s in mapfile.script_strings(raw, pos, names)]
        script_set = set(script)
        placed = {}
        named = []
        for tmpl, props, xyz in mapfile.objects(raw, pos, names):
            placed[tmpl.upper()] = placed.get(tmpl.upper(), 0) + 1
            nm = props.get('objectName', '')
            if nm:
                named.append((tmpl, nm, xyz))
        areas = [t for t, _, _ in mapfile.triggers(raw, pos, names)]

        def hidden(t):
            k = ' ' + resolved(db, t, 'kindof').upper() + ' '
            side = resolved(db, t, 'side').lower()
            return ' SELECTABLE ' not in k or side == 'civilian'

        scripted = sorted({s for s in script_set if s.upper() in db and hidden(s)})
        named_hidden = [(t, n, xyz) for t, n, xyz in named if t.upper() in db and hidden(t)]
        out.write(f'\n=== {folder}\n')
        flagged = 0
        for bonus, tag in tags:
            text = strs.get(tag.upper(), '?')
            verbs = sorted({v.lower() for v in VERBS.findall(text)})
            if verbs:
                flagged += 1
            out.write(f'  {"bonus " if bonus else ""}objective {tag}: {text}' + (f'   [{", ".join(verbs)}]' if verbs else '') + '\n')
        out.write(f'  hidden templates named by scripts ({len(scripted)}):\n')
        for t in scripted:
            disp = resolved(db, t, 'displayname')
            out.write(f'    {t}  display={strs.get(disp.upper(), disp)!r}  placed={placed.get(t.upper(), 0)}  '
                      f'kindof={resolved(db, t, "kindof")[:70]}\n')
        out.write(f'  hidden objects placed with a script name ({len(named_hidden)}):\n')
        for t, n, (x, y, z) in named_hidden[:60]:
            out.write(f'    {n!r} = {t} at ({x:.0f}, {y:.0f})\n')
        out.write(f'  trigger areas ({len(areas)}): ' + '; '.join(areas[:80]) + '\n')
        summary.append((folder, len(tags), flagged, len(scripted), len(named_hidden), len(areas)))
    out.write('\n\nSUMMARY: map, objectives, objectives needing a place or object, hidden scripted templates, '
              'hidden named objects, areas\n')
    for row in summary:
        out.write('  %s: %d objectives, %d flagged, %d hidden templates, %d hidden named, %d areas\n' % row)
    out.close()
    print(len(summary), 'missions audited')


if __name__ == '__main__':
    main()
