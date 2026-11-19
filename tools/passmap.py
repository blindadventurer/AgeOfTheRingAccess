"""Passability of a SAGE map (BFME2 / RotWK) around given points, offline.

Usage: python -I passmap.py <file.map> <aotr folder> x0 y0 x1 y1 [x y ...]
  Finds whether ground units can walk from the first point to each later point on the
  10-unit cell grid, prints the path to each reachable point as corner points, and a text
  picture of the area. Environment: CLIFF (default 9.8) slope limit, BASE_INI extra INI
  folders (';' separated, e.g. the base game's INI.big extracted) for templates the mod
  does not redefine.

Sources (formats after OpenSAGE's readers, read for method; independent code):
  HeightMapData v5: u32 width, u32 height, u32 border, u32 borderCount, borderCount x
    (u32 x, u32 y), u32 area, area x u16 heights (Council of Elrond: 610 x 635, border 30,
    6 borders, 68-byte header). Heights / 16 = world units (cave floor 1280, walls 3226).
  A cell is a cliff when its four corner heights differ by more than CLIFF world units
  (Zero Hour's pathfinder limit is 9.8). The impassability bits of BlendTileData are not
  used: read after the four u16 tile arrays they came out as regular stripes (2026-11-01).
  Objects: templates with KindOf IMMOBILE (not NO_COLLIDE) and a Geometry Box (major/minor
    half sizes, rotated by the object's angle, offset) or Cylinder/Sphere (major radius)
    block the cells whose centre they cover. ChildObject / ObjectReskin inherit from their
    parent; map.ini redefinitions are applied last.
World x = (cell x - border) * 10, the same for y.
"""
import math
import os
import re
import struct
import sys
from collections import deque

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mapfile  # noqa: E402

NUM = r'(-?\d+(?:\.\d+)?)'
SCENERY = re.compile(r'Module|BlackPlane|DeepFog|LightShaft|Fog\b', re.I)
BLOCK = re.compile(r'^\s*(Object|ChildObject|ObjectReskin)\s+(\S+)(?:[ \t]+(\S+))?(.*?)^End\b', re.M | re.S)


def chunk_data(raw, pos, names, wanted):
    for name, ver, start, end, _ in mapfile.walk(raw, pos, names):
        if name == wanted:
            return ver, start, end
    return None


def height_map(raw, pos, names):
    ver, p, end = chunk_data(raw, pos, names, 'HeightMapData')
    w, h, border, nb = struct.unpack_from('<IIII', raw, p)
    p += 16 + nb * 8
    area, = struct.unpack_from('<I', raw, p)
    p += 4
    heights = struct.unpack_from('<%dH' % area, raw, p)
    return w, h, border, heights


def geometries(aotr, map_ini, extra_dirs=()):
    """Template (upper) -> (shape, major, minor, offx, offy) for blocking templates."""
    raw = {}
    files = []
    for d in (os.path.join(aotr, 'data', 'ini'),) + tuple(extra_dirs):
        for root, _, fs in os.walk(d):
            files += [os.path.join(root, f) for f in fs if f.lower().endswith('.ini')]
    files.append(map_ini)
    for path in files:
        if not os.path.exists(path):
            continue
        text = open(path, 'rb').read().decode('latin-1')
        text = re.sub(r';[^\n]*', '', text)
        for m in BLOCK.finditer(text):
            kind, name, parent, body = m.group(1), m.group(2).upper(), (m.group(3) or '').upper(), m.group(4)
            if kind == 'Object':
                parent = ''
            f = {}
            k = re.search(r'^\s*KindOf\s*=([^\n]*)', body, re.M)
            if k:
                f['kindof'] = k.group(1)
            g = re.search(r'^\s*Geometry\s*=\s*(\w+)', body, re.M | re.I)
            if g:
                f['shape'] = g.group(1).upper()
            for key, field in (('maj', 'GeometryMajorRadius'), ('mnr', 'GeometryMinorRadius')):
                v = re.search(field + r'\s*=\s*' + NUM, body)
                if v:
                    f[key] = float(v.group(1))
            off = re.search(r'GeometryOffset\s*=\s*X:\s*' + NUM + r'\S*\s*Y:\s*' + NUM, body)
            if off:
                f['off'] = (float(off.group(1)), float(off.group(2)))
            old = raw.get(name)
            if old and path != map_ini:
                continue  # the mod's own definition (read first) wins over the base game's
            if old:
                merged = dict(old[1])
                merged.update(f)
                f, parent = merged, parent or old[0]
            raw[name] = (parent, f)

    def resolve(name, depth=0):
        if name not in raw or depth > 8:
            return {}
        parent, f = raw[name]
        out = dict(resolve(parent, depth + 1)) if parent else {}
        out.update(f)
        return out

    out = {}
    for name in raw:
        f = resolve(name)
        words = f.get('kindof', '').upper().split()
        if 'IMMOBILE' not in words or 'NO_COLLIDE' in words or 'shape' not in f:
            continue
        ox, oy = f.get('off', (0.0, 0.0))
        out[name] = (f['shape'], f.get('maj', 0.0), f.get('mnr', 0.0), ox, oy)
    return out


def placed(raw, pos, names):
    for name, ver, start, end, _ in mapfile.walk(raw, pos, names):
        if name != 'ObjectsList':
            continue
        r = mapfile.Reader(raw, start, end, names)
        while r.pos + 10 <= end:
            cname, cver, cstart, cend = r.chunk()
            if cname == 'Object':
                x, y, z = r.f32(), r.f32(), r.f32()
                angle = r.f32()
                r.u32()
                tmpl = r.astr()
                yield tmpl, x, y, angle
            r.pos = cend


def corners(path):
    """Cells where the path turns, so a route can be walked as a few straight legs."""
    if len(path) < 3:
        return path
    out = [path[0]]
    for a, b, c in zip(path, path[1:], path[2:]):
        if (b[0] - a[0], b[1] - a[1]) != (c[0] - b[0], c[1] - b[1]):
            out.append(b)
    out.append(path[-1])
    return out


class Grid:
    """The walkable cells of a map (cliffs, painted cells, blocking objects) for floods: used by
    make_gates.py to find which places a shut area cuts off. Same rules as main() below."""

    def __init__(self, mpath, aotr, raw=None, pos=None, names=None):
        if raw is None:
            raw, pos, names = mapfile.load(mpath)
        w, h, border, heights = height_map(raw, pos, names)
        self.w, self.h, self.border = w, h, border
        cliff = float(os.environ.get('CLIFF', '9.8'))
        blk = bytearray(w * h)
        for y in range(h - 1):
            row, nxt = y * w, (y + 1) * w
            for x in range(w - 1):
                a, b, c, d = heights[row + x], heights[row + x + 1], heights[nxt + x], heights[nxt + x + 1]
                if (max(a, b, c, d) - min(a, b, c, d)) / 16.0 > cliff:
                    blk[row + x] = 1
        ver, p, end = chunk_data(raw, pos, names, 'BlendTileData')
        rowb = (w + 7) // 8
        bits = raw[p + 4 + 14 * w * h:p + 4 + 14 * w * h + rowb * h]
        for y in range(h):
            for x in range(w):
                if bits[y * rowb + x // 8] >> (x % 8) & 1:
                    blk[y * w + x] = 1
        geo = geometries(aotr, os.path.join(os.path.dirname(mpath), 'map.ini'))
        for tmpl, x, y, ang in placed(raw, pos, names):
            g = geo.get(tmpl.split('/')[-1].upper())
            # Cave shells and black planes stand over the floor units walk on: Moria's start (281, 449)
            # lay under MoriaModuleDwarvenEntranceA (box 180) and the flood reached 0 cells (2026-11-15).
            if not g or SCENERY.search(tmpl):
                continue
            shape, maj, mnr, ox, oy = g
            ca, sa = math.cos(ang), math.sin(ang)
            cx, cy = x + ox * ca - oy * sa, y + ox * sa + oy * ca
            rad = max(maj, mnr) + 10
            for gy in range(int((cy - rad) / 10) + border, int((cy + rad) / 10) + border + 2):
                for gx in range(int((cx - rad) / 10) + border, int((cx + rad) / 10) + border + 2):
                    if not (0 <= gx < w and 0 <= gy < h):
                        continue
                    dx, dy = (gx - border) * 10 + 5 - cx, (gy - border) * 10 + 5 - cy
                    if shape == 'BOX':
                        lx, ly = dx * ca + dy * sa, -dx * sa + dy * ca
                        hit = abs(lx) <= maj and abs(ly) <= mnr
                    else:
                        hit = dx * dx + dy * dy <= maj * maj
                    if hit:
                        blk[gy * w + gx] = 1
        self.blk = blk

    def cell(self, x, y):
        gx, gy = int(x / 10) + self.border, int(y / 10) + self.border
        return gy * self.w + gx if 0 <= gx < self.w and 0 <= gy < self.h else -1

    def cells_in(self, pts):
        """Cell indices whose centre lies in the polygon pts."""
        from objective_places import inside
        xs, ys = [p[0] for p in pts], [p[1] for p in pts]
        out = []
        for gy in range(int(min(ys) / 10) + self.border, int(max(ys) / 10) + self.border + 1):
            for gx in range(int(min(xs) / 10) + self.border, int(max(xs) / 10) + self.border + 1):
                if 0 <= gx < self.w and 0 <= gy < self.h and \
                        inside((gx - self.border) * 10 + 5, (gy - self.border) * 10 + 5, pts):
                    out.append(gy * self.w + gx)
        return out

    def free_near(self, x, y, shut=None, radius=6):
        """A free cell within radius cells of the point (a place often stands on an object's cell)."""
        c = self.cell(x, y)
        if c < 0:
            return -1
        best, bd = -1, 1e9
        cx, cy = c % self.w, c // self.w
        for dy in range(-radius, radius + 1):
            for dx in range(-radius, radius + 1):
                gx, gy = cx + dx, cy + dy
                if 0 <= gx < self.w and 0 <= gy < self.h:
                    i = gy * self.w + gx
                    if not self.blk[i] and not (shut and shut[i]) and dx * dx + dy * dy < bd:
                        best, bd = i, dx * dx + dy * dy
        return best

    def flood(self, start, shut=None):
        """bytearray of reached cells from cell start (or a list of cells); shut: bytearray of extra
        blocked cells."""
        w, h, blk = self.w, self.h, self.blk
        seen = bytearray(w * h)
        if shut is not None:  # cells hold 0 or 1, so one big-integer OR joins them
            blk = (int.from_bytes(blk, 'little') | int.from_bytes(shut, 'little')).to_bytes(w * h, 'little')
        q = deque()
        for c in (start if isinstance(start, list) else [start]):
            if c >= 0 and not blk[c] and not seen[c]:
                seen[c] = 1
                q.append(c)
        steps = (1, -1, w, -w, w + 1, w - 1, -w + 1, -w - 1)
        while q:
            c = q.popleft()
            x = c % w
            for d in steps:
                n = c + d
                if 0 <= n < w * h and not seen[n] and not blk[n] and abs(n % w - x) <= 1:
                    seen[n] = 1
                    q.append(n)
        return seen


def main():
    mpath, aotr = sys.argv[1], sys.argv[2]
    pts = [(float(sys.argv[i]), float(sys.argv[i + 1])) for i in range(3, len(sys.argv) - 1, 2)]
    raw, pos, names = mapfile.load(mpath)
    w, h, border, heights = height_map(raw, pos, names)
    cliff = float(os.environ.get('CLIFF', '9.8'))
    blocked = bytearray(w * h)
    for y in range(h - 1):
        for x in range(w - 1):
            c = (heights[y * w + x], heights[y * w + x + 1], heights[(y + 1) * w + x], heights[(y + 1) * w + x + 1])
            if (max(c) - min(c)) / 16.0 > cliff:
                blocked[y * w + x] = 1
    # The designer-painted impassability bits: BlendTileData v18 holds 14 bytes per cell
    # before them (found 2026-11-01 by matching every candidate offset against the slope
    # cliffs: offset 4 + 14 * cells matched 47 %, the next best 36 %), then ceil(width / 8)
    # bytes per row, low bit first. IMPASS=0 leaves them out.
    painted = 0
    if os.environ.get('IMPASS', '1') != '0':
        ver, p, end = chunk_data(raw, pos, names, 'BlendTileData')
        row = (w + 7) // 8
        bits = raw[p + 4 + 14 * w * h:p + 4 + 14 * w * h + row * h]
        for y in range(h):
            for x in range(w):
                if bits[y * row + x // 8] >> (x % 8) & 1:
                    if not blocked[y * w + x]:
                        painted += 1
                    blocked[y * w + x] = 1
    print(f'painted impassable cells (not cliffs by slope): {painted}')
    extra = [d for d in os.environ.get('BASE_INI', '').split(';') if d]
    geo = geometries(aotr, os.path.join(os.path.dirname(mpath), 'map.ini'), extra)
    obst = bytearray(w * h)
    used = {}
    for tmpl, x, y, ang in placed(raw, pos, names):
        g = geo.get(tmpl.split('/')[-1].upper())
        if not g:
            continue
        shape, maj, mnr, ox, oy = g
        ca, sa = math.cos(ang), math.sin(ang)
        cx, cy = x + ox * ca - oy * sa, y + ox * sa + oy * ca
        rad = max(maj, mnr) + 10
        used[tmpl] = used.get(tmpl, 0) + 1
        for gy in range(int((cy - rad) / 10) + border, int((cy + rad) / 10) + border + 2):
            for gx in range(int((cx - rad) / 10) + border, int((cx + rad) / 10) + border + 2):
                if not (0 <= gx < w and 0 <= gy < h):
                    continue
                dx, dy = (gx - border) * 10 + 5 - cx, (gy - border) * 10 + 5 - cy
                if shape == 'BOX':
                    lx, ly = dx * ca + dy * sa, -dx * sa + dy * ca
                    inside = abs(lx) <= maj and abs(ly) <= mnr
                else:
                    inside = dx * dx + dy * dy <= maj * maj
                if inside:
                    obst[gy * w + gx] = 1
    print(f'map {w}x{h} border {border}; cliff cells {sum(blocked)}; obstacle cells {sum(obst)} from '
          f'{sum(used.values())} objects of {len(used)} templates')

    def cell(p):
        return int(p[0] / 10) + border, int(p[1] / 10) + border

    def world(c):
        return (c[0] - border) * 10 + 5, (c[1] - border) * 10 + 5

    def free(gx, gy, objects=True):
        i = gy * w + gx
        return 0 <= gx < w and 0 <= gy < h and not blocked[i] and not (objects and obst[i])

    start = cell(pts[0])
    for objects in (True, False):
        seen = {start: None}
        q = deque([start])
        while q:
            c = q.popleft()
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1), (1, 1), (1, -1), (-1, 1), (-1, -1)):
                n = (c[0] + dx, c[1] + dy)
                if n not in seen and free(*n, objects=objects):
                    seen[n] = c
                    q.append(n)
        label = 'terrain + objects' if objects else 'terrain only'
        print(f'{label}: {len(seen)} cells reachable from {pts[0]} (start cell free: {free(*start, objects=objects)})')
        for p in pts[1:]:
            c = cell(p)
            if c in seen:
                path, k = [], c
                while k:
                    path.append(k)
                    k = seen[k]
                path.reverse()
                turns = ' -> '.join('(%d, %d)' % world(t) for t in corners(path))
                print(f'  {p}: reachable, {len(path)} cells: {turns}')
            else:
                best = min(seen, key=lambda s: (s[0] - c[0]) ** 2 + (s[1] - c[1]) ** 2)
                bw = world(best)
                print(f'  {p}: NOT reachable; nearest reachable cell at {bw}, '
                      f'{math.hypot(bw[0] - p[0], bw[1] - p[1]):.0f} units away')
    # Picture: '#' cliff, 'o' obstacle, '.' free, digits = the given points; north at the top.
    xs = [p[0] for p in pts]
    ys = [p[1] for p in pts]
    x0, x1 = int(min(xs) / 10) - 8 + border, int(max(xs) / 10) + 8 + border
    y0, y1 = int(min(ys) / 10) - 8 + border, int(max(ys) / 10) + 8 + border
    marks = {cell(p): str(i) for i, p in enumerate(pts)}
    print(f'picture: x {(x0 - border) * 10}..{(x1 - border) * 10}, y {(y1 - border) * 10} (top) .. {(y0 - border) * 10}')
    for gy in range(y1, y0 - 1, -1):
        line = ''
        for gx in range(x0, x1 + 1):
            if (gx, gy) in marks:
                line += marks[(gx, gy)]
            elif not (0 <= gx < w and 0 <= gy < h):
                line += ' '
            elif blocked[gy * w + gx]:
                line += '#'
            elif obst[gy * w + gx]:
                line += 'o'
            else:
                line += '.'
        print(line)


if __name__ == '__main__':
    main()
