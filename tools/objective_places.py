"""For every campaign mission: which places each objective needs (areas and waypoints).

Usage: python -I objective_places.py <aotr folder> <out.txt> [map filter]

Method (scripts parsed by mapfile.scripts):
  1. The scripts that complete objective N: MARK_MISSION_OBJECTIVE_COMPLETED with int N,
     or DISPLAY_NOTIFICATION_BOX naming its tag.
  2. Their conditions; a FLAG / TIMER / COUNTER / script name condition is followed to the
     scripts whose actions set it (up to 4 steps back).
  3. Every trigger area or waypoint named in those conditions is a candidate place, listed
     with its centre and the condition that tests it (e.g. NAMED_INSIDE_AREA Bilbo).
Objectives whose chain names no place are fights or timers.
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mapfile  # noqa: E402
from missions_audit import read_str  # noqa: E402

PLACE_CONDS = re.compile(r'INSIDE_AREA|ENTERED_AREA|EXITED_AREA|AREA|NEAR|WAYPOINT|DISTANCE', re.I)
STATE = re.compile(r'FLAG|TIMER|COUNTER|SCRIPT|SUBROUTINE', re.I)


def inside(x, y, pts):
    """Point in polygon (even-odd rule) on the x, y of the area's points."""
    c = False
    for i in range(len(pts)):
        x1, y1 = pts[i][0], pts[i][1]
        x2, y2 = pts[i - 1][0], pts[i - 1][1]
        if (y1 > y) != (y2 > y) and x < (x2 - x1) * (y - y1) / (y2 - y1) + x1:
            c = not c
    return c


def analyse(path, strs, out):
    raw, pos, names = mapfile.load(path)
    wps = mapfile.waypoints(raw, pos, names)
    # An area's point: the waypoint inside it nearest its centre, else the centre. The centre
    # can sit on rock: Council of Elrond's "Bilbo - End Trigger" centre (1278, 1223) is next to
    # a RockArch01, a move order there took Bilbo to the outside of the cliffs (2026-11-01), while
    # the dwarves' escape path ends inside the area at (1317, 1182).
    areas = {}
    for t, _, pts in mapfile.triggers(raw, pos, names):
        if not pts:
            continue
        cx, cy = sum(p[0] for p in pts) / len(pts), sum(p[1] for p in pts) / len(pts)
        inner = [w for w in wps.values() if inside(w[0], w[1], pts)]
        areas[t] = min(inner, key=lambda w: (w[0] - cx) ** 2 + (w[1] - cy) ** 2) if inner else (cx, cy)
    sc = list(mapfile.scripts(raw, pos, names))
    setters = {}  # string -> scripts whose actions name it
    for s in sc:
        for name, strings, args in s['actions'] + s['false']:
            # only actions that change script state: flags, timers, counters, enabling scripts
            if not STATE.search(name):
                continue
            for v in strings:
                for key in {v, v.split('/')[-1]}:  # flags: "PlyrCreeps/Bilbo - Complete"
                    lst = setters.setdefault(key, [])
                    if not lst or lst[-1] is not s:
                        lst.append(s)
        setters.setdefault(s['name'], [])
    by_name = {s['name']: s for s in sc}
    # scripts enabled by name: ENABLE_SCRIPT / CALL_SUBROUTINE in other scripts' actions
    return areas, wps, sc, setters, by_name


def chain_for(obj_index, tag, sc, setters):
    """The scripts that complete objective obj_index and those they wait for: [(script, depth)].
    The completing script shows "objective completed" with this tag; else it is the one with
    MARK_MISSION_OBJECTIVE_COMPLETED whose int is the 1-based index (Council of Elrond:
    "Objetive 00 - Complete" marks 1 = the first tag, "Objetive 00 - Boromir - Complete" marks 2).
    From there FLAG / TIMER / COUNTER / script-name conditions and script enablers are followed
    back up to 4 steps."""
    done = [s for s in sc if any(a[0] == 'MARK_MISSION_OBJECTIVE_COMPLETED' for a in s['actions'])
            and any(a[0] == 'DISPLAY_NOTIFICATION_BOX' and tag in a[1] for a in s['actions'])]
    if not done:
        done = [s for s in sc if any(a[0] == 'MARK_MISSION_OBJECTIVE_COMPLETED' and a[2] and a[2][0][1] == obj_index + 1
                                     for a in s['actions'])]
    chain, seen, frontier = [], set(), [(s, 0) for s in done]
    while frontier:
        s, depth = frontier.pop(0)
        if s['name'] in seen:
            continue
        seen.add(s['name'])
        chain.append((s, depth))
        if depth >= 4:
            continue
        for cname, strings, args in s['conditions']:
            if STATE.search(cname):
                for v in strings:
                    for t in setters.get(v, []) + setters.get(v.split('/')[-1], []):
                        if t['name'] not in seen:
                            frontier.append((t, depth + 1))
        # a script enabled by another: follow the enablers too
        for t in setters.get(s['name'], []):
            if t['name'] not in seen:
                frontier.append((t, depth + 1))
    return done, chain


def places_for(obj_index, tag, sc, setters, areas, wps):
    done, chain = chain_for(obj_index, tag, sc, setters)
    found = []
    for s, depth in chain:
        for cname, strings, args in s['conditions']:
            for v in strings:
                if v in ('Player_1', 'ALL'):
                    continue
                if v in areas:
                    found.append((v, areas[v], cname, [x for x in strings if x != v], depth, s['name']))
                elif v in wps:
                    found.append((v, wps[v], cname, [x for x in strings if x != v], depth, s['name']))
    return done, found


def main():
    aotr, out_path = sys.argv[1], sys.argv[2]
    flt = sys.argv[3].lower() if len(sys.argv) > 3 else ''
    lotr = read_str(os.path.join(aotr, 'data', 'lotr.str'))
    maps = os.path.join(aotr, 'maps')
    out = open(out_path, 'w', encoding='utf-8')
    for folder in sorted(os.listdir(maps)):
        if flt and flt not in folder.lower():
            continue
        mdir = os.path.join(maps, folder)
        ini, mpath = os.path.join(mdir, 'map.ini'), os.path.join(mdir, folder + '.map')
        if not (os.path.exists(ini) and os.path.exists(mpath)):
            continue
        text = open(ini, 'rb').read().decode('latin-1')
        if 'MissionObjectiveList' not in text:
            continue
        strs = dict(lotr)
        strs.update(read_str(os.path.join(mdir, 'map.str')))
        tags = [t for _, t in re.findall(r'(Bonus)?MissionObjectiveTag\s*=\s*(\S+)', text)]
        try:
            areas, wps, sc, setters, by_name = analyse(mpath, strs, out)
        except Exception as e:  # noqa: BLE001
            out.write(f'\n=== {folder}: unreadable ({e})\n')
            continue
        out.write(f'\n=== {folder}: {len(sc)} scripts, {len(areas)} areas, {len(wps)} waypoints\n')
        for i, tag in enumerate(tags):
            done, found = places_for(i, tag, sc, setters, areas, wps)
            out.write(f'  [{i}] {tag}: {strs.get(tag.upper(), "?")}\n')
            out.write(f'      completed by: {", ".join(s["name"] for s in done[:4]) or "-"}\n')
            seen = set()
            for v, (x, y), cname, other, depth, via in found:
                if v in seen:
                    continue
                seen.add(v)
                out.write(f'      place: {v!r} ({x:.0f}, {y:.0f})  {cname} {other[:3]}  step {depth} via {via!r}\n')
    out.close()


if __name__ == '__main__':
    main()
