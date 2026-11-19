"""Read a SAGE .map file (BFME2 / RotWK): objects, polygon triggers, scripts.

Usage:
  python -I mapfile.py <file.map> chunks              list chunk names and sizes
  python -I mapfile.py <file.map> objects [filter]    placed objects: template, name, x y z
  python -I mapfile.py <file.map> triggers [filter]   polygon triggers (areas) with their points
  python -I mapfile.py <file.map> scripts [filter]    scripts with conditions and actions (strings only)
  python -I mapfile.py <file.map> strings [filter]    every length-prefixed ASCII string of the script chunks

Format: "EAR\\0" + u32 size + RefPack data (or raw "CkMp"); then "CkMp", u32 count,
count x {u8 len, name, u32 id}; then chunks {u32 id, u16 version, u32 size, data}.
Layouts follow OpenSAGE's MapFile readers (read for method; this is an independent
reader). Unknown chunk contents are skipped by size.
"""
import struct
import sys


def refpack(data, pos):
    flags = data[pos]
    pos += 2  # 0x10 0xFB (or 0x90 0xFB with 4-byte sizes)
    pos += 4 if flags & 0x80 else 3
    out = bytearray()
    while pos < len(data):
        b0 = data[pos]
        if b0 < 0x80:
            b1 = data[pos + 1]
            pos += 2
            plen, clen, off = b0 & 3, ((b0 & 0x1C) >> 2) + 3, ((b0 & 0x60) << 3) + b1 + 1
        elif b0 < 0xC0:
            b1, b2 = data[pos + 1], data[pos + 2]
            pos += 3
            plen, clen, off = (b1 >> 6) & 3, (b0 & 0x3F) + 4, ((b1 & 0x3F) << 8) + b2 + 1
        elif b0 < 0xE0:
            b1, b2, b3 = data[pos + 1], data[pos + 2], data[pos + 3]
            pos += 4
            plen, clen = b0 & 3, ((b0 & 0x0C) << 6) + b3 + 5
            off = ((b0 & 0x10) << 12) + (b1 << 8) + b2 + 1
        elif b0 < 0xFC:
            pos += 1
            plen, clen, off = ((b0 & 0x1F) << 2) + 4, 0, 0
        else:
            pos += 1
            out += data[pos:pos + (b0 & 3)]
            break
        out += data[pos:pos + plen]
        pos += plen
        for _ in range(clen):
            out.append(out[-off])
    return bytes(out)


def load(path):
    raw = open(path, 'rb').read()
    if raw[:4] == b'EAR\0':
        raw = refpack(raw, 8)
    if raw[:4] != b'CkMp':
        raise SystemExit('not a CkMp map')
    pos = 4
    count, = struct.unpack_from('<I', raw, pos)
    pos += 4
    names = {}
    for _ in range(count):
        n = raw[pos]
        name = raw[pos + 1:pos + 1 + n].decode('latin-1')
        cid, = struct.unpack_from('<I', raw, pos + 1 + n)
        names[cid] = name
        pos += 1 + n + 4
    return raw, pos, names


class Reader:
    def __init__(self, raw, pos, end, names):
        self.raw, self.pos, self.end, self.names = raw, pos, end, names

    def u8(self):
        v = self.raw[self.pos]; self.pos += 1; return v

    def u16(self):
        v, = struct.unpack_from('<H', self.raw, self.pos); self.pos += 2; return v

    def u32(self):
        v, = struct.unpack_from('<I', self.raw, self.pos); self.pos += 4; return v

    def i32(self):
        v, = struct.unpack_from('<i', self.raw, self.pos); self.pos += 4; return v

    def f32(self):
        v, = struct.unpack_from('<f', self.raw, self.pos); self.pos += 4; return v

    def astr(self):
        n = self.u16(); s = self.raw[self.pos:self.pos + n].decode('latin-1'); self.pos += n; return s

    def ustr(self):
        n = self.u16(); s = self.raw[self.pos:self.pos + 2 * n].decode('utf-16-le', 'replace'); self.pos += 2 * n; return s

    def chunk(self):
        cid = self.u32(); ver = self.u16(); size = self.u32()
        return self.names.get(cid, '?%d' % cid), ver, self.pos, self.pos + size

    def dict(self):
        n = self.u16()
        out = {}
        for _ in range(n):
            key = self.u32()
            kind = key & 0xFF
            name = self.names.get(key >> 8, '?')
            if kind == 0: v = bool(self.u8())
            elif kind == 1: v = self.i32()
            elif kind == 2: v = self.f32()
            elif kind == 3: v = self.astr()
            elif kind == 4: v = self.ustr()
            else: raise ValueError('dict kind %d' % kind)
            out[name] = v
        return out


def walk(raw, pos, names, end=None, depth=0):
    """Yields (name, version, start, end, depth) for every chunk, nested ones too."""
    end = len(raw) if end is None else end
    containers = {'ObjectsList', 'PolygonTriggers', 'PlayerScriptsList', 'ScriptList', 'ScriptGroup',
                  'SidesList', 'WaypointsList', 'GlobalLighting'}
    r = Reader(raw, pos, end, names)
    while r.pos + 10 <= end:
        name, ver, start, cend = r.chunk()
        if cend > end or cend < start:
            return
        yield name, ver, start, cend, depth
        r.pos = cend


def objects(raw, pos, names):
    for name, ver, start, end, _ in walk(raw, pos, names):
        if name != 'ObjectsList':
            continue
        r = Reader(raw, start, end, names)
        while r.pos + 10 <= end:
            cname, cver, cstart, cend = r.chunk()
            if cname == 'Object':
                x, y, z = r.f32(), r.f32(), r.f32()
                angle = r.f32()
                flags = r.u32()
                tmpl = r.astr()
                props = r.dict()
                yield tmpl, props, (x, y, z)
            r.pos = cend


def triggers(raw, pos, names):
    for name, ver, start, end, _ in walk(raw, pos, names):
        if name == 'TriggerAreas':
            # BFME2 v1 (Council of Elrond, read 2026-11-01): u32 count; each: astr name,
            # astr (empty), u32 id, u32 n, n x (f32 x, f32 y), u32 (0)
            r = Reader(raw, start, end, names)
            count = r.u32()
            for _ in range(count):
                tname = r.astr()
                r.astr()
                tid = r.u32()
                npts = r.u32()
                if npts > 10000:
                    return
                pts = [(r.f32(), r.f32(), 0) for _ in range(npts)]
                r.u32()
                yield tname, tid, pts
            continue
        if name != 'PolygonTriggers':
            continue
        r = Reader(raw, start, end, names)
        count = r.u32()
        for _ in range(count):
            tname = r.astr()
            layer = r.astr() if ver >= 4 else ''
            tid = r.u32()
            water = r.u8()
            river = r.u8() if ver >= 3 else 0
            rstart = r.u32() if ver >= 3 else 0
            if ver >= 5:
                r.pos += 0  # some versions carry extra fields; points count follows
            npts = r.u32()
            if npts > 10000:
                return
            pts = [(r.i32(), r.i32(), r.i32()) for _ in range(npts)]
            yield tname, tid, pts


ID_SCRIPTLIST, ID_GROUP, ID_SCRIPT = 'ScriptList', 'ScriptGroup', 'Script'


def _strings_in(raw, start, end):
    """Length-prefixed printable ASCII strings inside a byte range."""
    out, p = [], start
    while p + 2 < end:
        n, = struct.unpack_from('<H', raw, p)
        if 1 <= n <= 200 and p + 2 + n <= end:
            s = raw[p + 2:p + 2 + n]
            if all(32 <= c < 127 for c in s):
                out.append(s.decode('latin-1'))
                p += 2 + n
                continue
        p += 1
    return out


def _children(raw, start, end, ids):
    """Contiguous child chunks starting at the first valid header whose id is in ids."""
    p = start
    while p + 10 <= end:
        cid, = struct.unpack_from('<I', raw, p)
        if cid in ids:
            ver, size = struct.unpack_from('<HI', raw, p + 4)
            if ver < 20 and p + 10 + size <= end:
                break
        p += 1
    out = []
    while p + 10 <= end:
        cid, = struct.unpack_from('<I', raw, p)
        ver, size = struct.unpack_from('<HI', raw, p + 4)
        if p + 10 + size > end:
            break
        out.append((cid, ver, p + 10, p + 10 + size))
        p += 10 + size
    return out


def scripts(raw, pos, names):
    """Every script: {'name', 'group', 'conditions': [(name, [strings])], 'actions': [...], 'false': [...]}.
    Layout read from Council of Elrond 2026-11-01: Script v4 = astr name, 3 astr comments, flags,
    then OrCondition chunks (each holding Condition chunks) and ScriptAction / ScriptActionFalse
    chunks; a condition or action starts with u32 type, u32 name key (name table id << 8 | 3),
    u32 argument count, then its arguments (strings collected here, not decoded one by one)."""
    inv = {v: k for k, v in names.items()}
    i_list, i_group, i_script = inv.get('ScriptList'), inv.get('ScriptGroup'), inv.get('Script')
    i_or, i_cond = inv.get('OrCondition'), inv.get('Condition')
    i_act, i_false = inv.get('ScriptAction'), inv.get('ScriptActionFalse')

    def item(start, end):
        """(name, strings, args): args are (type, int, float, string) as stored: u32 type,
        i32, f32, astr (MARK_MISSION_OBJECTIVE_COMPLETED keeps the objective's index in
        its int: 1 for Council of Elrond's Boromir objective, the 2nd in map.ini)."""
        key, = struct.unpack_from('<I', raw, start + 4)
        count, = struct.unpack_from('<I', raw, start + 8)
        args, p = [], start + 12
        for _ in range(min(count, 32)):
            if p + 14 > end:
                break
            t, iv, fv = struct.unpack_from('<Iif', raw, p)
            n, = struct.unpack_from('<H', raw, p + 12)
            if p + 14 + n > end:
                break
            sv = raw[p + 14:p + 14 + n].decode('latin-1')
            args.append((t, iv, fv, sv))
            p += 14 + n
        return names.get(key >> 8, '?%X' % key), _strings_in(raw, start + 12, end), args

    def inverted(start, end, ver):
        """Condition v6 ends with u32 1 and u32 inverted (map good taming of smeagol 2026-11-07: 45 of
        1192 conditions; "G Nazgul Attack" tests Frodo NOT INSIDE_GARRISON). Earlier readers missed it."""
        count, = struct.unpack_from('<I', raw, start + 8)
        p = start + 12
        for _ in range(min(count, 32)):
            if p + 14 > end:
                return False
            n, = struct.unpack_from('<H', raw, p + 12)
            p += 14 + n
        return ver >= 6 and p + 8 <= end and struct.unpack_from('<I', raw, p + 4)[0] != 0

    def script(start, end, group):
        n, = struct.unpack_from('<H', raw, start)
        name = raw[start + 2:start + 2 + n].decode('latin-1')
        # 'not' and 'clause' run beside 'conditions': inverted or not, and which OrCondition (the
        # clauses are ORed, the conditions inside one ANDed).
        s = {'name': name, 'group': group, 'conditions': [], 'not': [], 'clause': [], 'actions': [], 'false': []}
        for cid, ver, cs, ce in _children(raw, start + 2 + n, end, {i_or, i_act, i_false}):
            if cid == i_or:
                clause = len(set(s['clause']))
                for c2, v2, s2, e2 in _children(raw, cs, ce, {i_cond}):
                    s['conditions'].append(item(s2, e2))
                    s['not'].append(inverted(s2, e2, v2))
                    s['clause'].append(clause)
            elif cid == i_act:
                s['actions'].append(item(cs, ce))
            elif cid == i_false:
                s['false'].append(item(cs, ce))
        return s

    def walk_list(start, end, group):
        for cid, ver, cs, ce in _children(raw, start, end, {i_group, i_script}):
            if cid == i_script:
                yield script(cs, ce, group)
            elif cid == i_group:
                n, = struct.unpack_from('<H', raw, cs)
                gname = raw[cs + 2:cs + 2 + n].decode('latin-1')
                yield from walk_list(cs + 2 + n, ce, (group + '/' if group else '') + gname)

    for name, ver, start, end, _ in walk(raw, pos, names):
        if name != 'PlayerScriptsList':
            continue
        for cid, ver2, ls, le in _children(raw, start, end, {i_list}):
            if le > ls:
                yield from walk_list(ls, le, '')


def teams(raw, pos, names):
    """Team name -> owning side, from the Teams chunk (v1: u32 count, then a property dict each)."""
    out = {}
    for name, ver, start, end, _ in walk(raw, pos, names):
        if name == 'Teams':
            r = Reader(raw, start, end, names)
            for _ in range(r.u32()):
                d = r.dict()
                if d.get('teamName'):
                    out[d['teamName']] = d.get('teamOwner', '')
    return out


def waypoints(raw, pos, names):
    """Waypoint objects (template *Waypoint*) by name: {name: (x, y)}."""
    out = {}
    for tmpl, props, (x, y, z) in objects(raw, pos, names):
        nm = props.get('waypointName')
        if nm:
            out[nm] = (x, y)
    return out


def script_strings(raw, pos, names):
    """Every length-prefixed ASCII string inside the script chunks, in file order."""
    for name, ver, start, end, _ in walk(raw, pos, names):
        if name != 'PlayerScriptsList':
            continue
        p = start
        while p + 2 < end:
            n, = struct.unpack_from('<H', raw, p)
            if 3 <= n <= 200 and p + 2 + n <= end:
                s = raw[p + 2:p + 2 + n]
                if all(32 <= c < 127 for c in s):
                    yield p, s.decode('latin-1')
                    p += 2 + n
                    continue
            p += 1


def main():
    path, cmd = sys.argv[1], sys.argv[2]
    flt = sys.argv[3].lower() if len(sys.argv) > 3 else ''
    raw, pos, names = load(path)
    if cmd == 'chunks':
        for name, ver, start, end, d in walk(raw, pos, names):
            print(f'{name} v{ver} {end - start} bytes')
    elif cmd == 'objects':
        for tmpl, props, (x, y, z) in objects(raw, pos, names):
            line = f'{tmpl} name={props.get("objectName", "")!r} at ({x:.0f}, {y:.0f}, {z:.0f}) team={props.get("originalOwner", "")}'
            if flt in line.lower():
                print(line)
    elif cmd == 'triggers':
        for tname, tid, pts in triggers(raw, pos, names):
            if flt in tname.lower():
                xs = [p[0] for p in pts]; ys = [p[1] for p in pts]
                cx = sum(xs) / len(xs) if xs else 0; cy = sum(ys) / len(ys) if ys else 0
                print(f'{tname} id {tid} centre ({cx:.0f}, {cy:.0f}) points {len(pts)}')
    elif cmd in ('scripts', 'strings'):
        for p, s in script_strings(raw, pos, names):
            if flt in s.lower():
                print(f'{p:08X} {s}')


if __name__ == '__main__':
    main()
