"""Write data\\gates.txt: what opens each way a mission keeps shut (HUMAN_IMPASSABLE_AREA), where to go
to open it, and which guide places each shut way cuts off.

Usage: python -I make_gates.py <aotr folder> <out.txt> <report.txt> [--no-reach] [map filter]

An objective's scripts often never name what opens the road: Council of Elrond's "Explore the cave"
needs the Ring, Mirkwood's rescue needs Balin first, Westfold's keep opens after six attacker teams
fall. Each area a script closes and another opens is a gate.

What opens it: every non-debug script that lifts the area is a candidate. The walk goes back from each
through flags, timers, counters and enabling (triggers.py) to the first thing the player can do. Two
rules pick the candidate (Westfold, 2026-11-14: the old "smallest depth" rule named the captain's
cinematic, which itself needs a place inside the shut town, for every area):
  - a candidate whose act is reaching a place inside the very area it opens can never fire first;
  - of the rest, the one with the shortest chain of earlier events comes first in the story.
A counter condition ("Counter - Death Counter Peasant Attackers" >= 2) whose increments come from the
on-destroyed script of teams reads "разбить 2 отряда врагов", with the points where those teams start
(the area that spawns them, then their spawn waypoint) as places to go.

Which places it cuts off (passmap.Grid floods from the player's start, 10-unit cells): a guide place
"needs" an area when it is reached with every gate open but not with that area alone shut; when the
gates shut at the start together cut it off and no single one does, the start gates whose opening alone
lets the flood in are listed as "any" (two town gates: either one will do).

Format, after "[map folder]":
area | x1 y1 x2 y2 | what opens it | go x y; x y | poly x y; x y; ...
@ x y | need area; area | any area; area
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import make_guides as G  # noqa: E402
import mapfile  # noqa: E402
import passmap  # noqa: E402
import triggers as T  # noqa: E402
from missions_audit import read_str  # noqa: E402
from objective_places import inside  # noqa: E402

# Words read by hand where the walk back gives the wrong act. Places to go still come from the tool.
HAND = {
    # map good westfold (read 2026-11-14): the town opens by "Counter - Death Counter Peasant Attackers"
    # (+1 per attacker team gone, "PS Peasant Attackers"). Waves start at "Area - Castle South Inner".
    'Area - Castle Centre Inner': 'войти в город через южные ворота и разбить там первые 2 отряда врагов',
    'Area - Impassible Plateu': 'разбить в городе 4 отряда врагов',
    'Area - Impassible Centre Approach': 'разбить в городе 4 отряда врагов',
    'Area - Castle Keep': 'разбить в городе все 6 отрядов врагов, начиная от южных ворот',
    "Bilbo - Gollum's Cave Entry": 'подобрать Кольцо',
    # map good isengard (read 2026-11-15): "Castle - All Buildings Destroyed" waits for "Counter - Buildings
    # Alive" <= 0, a counter the buildings' deaths lower, then Treebeard's "To the Dam!".
    'Area - Impassible Dam': 'разрушить все постройки Изенгарда',
    'Area - Mirkwood - Spiders - Impassible 01': 'дождаться конца разговора',
    'Area - Mirkwood - Spiders - Impassible 03': 'освободить всех гномов',
    'Gladden - Area 01 - North': 'найти Исилдура героем Элендуром',
    'Gladden - Area 01 - South': 'найти Исилдура героем Элендуром',
    'Gladden - Area 02 - North': 'найти Исилдура героем Элендуром',
    'Gladden - Area 02 - South': 'найти Исилдура героем Элендуром',
    'Impassible Area - Gladden 01': 'найти Исилдура героем Элендуром',
    'Impassible Area - Gladden 02': 'найти Исилдура героем Элендуром',
    # map good lothlorien: Cutoff and the bridges open at "Bridge Attack - Complete", two objectives on;
    # "разбить: Bridge Assault" sent the player to a fight that does not exist yet (2026-11-07 01:23:34).
    'Impassible - Starting Phase Cutoff': 'спасти Братство на юге, потом разбить орков у южной переправы',
    'Lothlorien - Bridge North West': 'спасти Братство на юге, потом разбить орков у южной переправы',
    'Lothlorien - Bridge South West': 'спасти Братство на юге, потом разбить орков у южной переправы',
    'Lothlorien - Bridge South East': 'спасти Братство на юге, потом разбить орков у южной переправы',
    'Lothlorien - Reveal Trigger': 'разбить всех орков у реки и шамана Уршака',
    'Impassible - Fellowship Ford': 'разбить первые отряды орков у реки',
    # Lothlorien's goblin caves: the walk back names "Force North", unproven; the mission was won without
    # a word here (2026-11-07), so none is given (empty words: "откроется по ходу миссии").
    'Gob Cave 01': '', 'Gob Cave 02': '', 'Gob Cave 03': '', 'Gob Cave 04': '', 'Gob Cave 05': '',
}

SKIP = re.compile(r'debug|\bfail\b|\breset\b|\btest\b', re.I)
CREATE = ('CREATE_UNNAMED_ON_TEAM_AT_WAYPOINT', 'CREATE_NAMED_ON_TEAM_AT_WAYPOINT', 'CREATE_REINFORCEMENT_TEAM')


def shuts(action, value):
    return action[0] == 'HUMAN_IMPASSABLE_AREA' and any(a[0] == 8 for a in action[2]) and \
        any(a[0] == 8 and bool(a[1]) == value for a in action[2])


def area_of(action):
    return next((a[3] for a in action[2] if a[3]), '')


def top(s):
    return s['group'].split('/')[0]


GENERIC = {'', 'Objectives', 'Audio', 'Setup', 'Cinematics', 'Mission Runner', 'Debug'}


def teams_word(n):
    if n % 10 == 1 and n % 100 != 11:
        return 'отряд'
    return 'отряда' if n % 10 in (2, 3, 4) and n % 100 not in (12, 13, 14) else 'отрядов'


class Map:
    """A mission plus what the gate tool needs beside triggers.Mission."""

    def __init__(self, aotr, folder, lotr, names):
        self.m = m = T.Mission(aotr, folder, lotr)
        self.names = names
        mdir = os.path.join(aotr, 'maps', folder)
        names.for_map(mdir, m.strs)
        raw, pos, nm = mapfile.load(os.path.join(mdir, folder + '.map'))
        self.raw = (raw, pos, nm)
        self.by_tmpl = {}  # template (upper) -> [(x, y)] placed
        self.own = []      # units placed on the player's side
        for tmpl, props, (x, y, z) in mapfile.objects(raw, pos, nm):
            self.by_tmpl.setdefault(tmpl.split('/')[-1].upper(), []).append((x, y))
            if props.get('originalOwner', '').startswith(m.me + '/'):
                self.own.append((x, y))
        # Which scripts create a team's units, in file order (the waves of Westfold: Spawn 01, 02, 03).
        self.spawner = {}
        for i, s in enumerate(m.scripts):
            if SKIP.search(s['name'] + ' ' + s['group']):
                continue
            for a, st, args in s['actions']:
                ss = [x[3] for x in args if x[3]]
                team = ss[1] if a == 'CREATE_UNNAMED_ON_TEAM_AT_WAYPOINT' and len(ss) >= 3 else \
                    ss[2] if a == 'CREATE_NAMED_ON_TEAM_AT_WAYPOINT' and len(ss) >= 4 else \
                    ss[0] if a == 'CREATE_REINFORCEMENT_TEAM' and ss else ''
                if team:
                    self.spawner.setdefault(T.bare(team), []).append((i, s))

    def reach_point(self, s):
        """The first place the player must walk to so that script s runs, or None."""
        m = self.m
        for n in sorted(m.needs(m.trace([s], set())), key=lambda v: v['depth']):
            if n['depth'] <= 6 and not n['bad'] and n['kind'] == 'reach' and n['side'] == 'player' and n.get('x'):
                return (n['x'], n['y'], G.spoken(n['what']))
        return None

    def team_points(self, teams):
        """Where counted teams start, in the order the scripts bring them in: the area whose entry
        spawns them, then their spawn waypoint; or where their units are placed."""
        m = self.m
        order = []
        for k, team in enumerate(teams):
            sp = self.spawner.get(T.bare(team), [])
            order.append((sp[0][0] if sp else -1, k, team))
        pts = []
        for _, _, team in sorted(order):
            for i, s in self.spawner.get(T.bare(team), [])[:1]:
                p = self.reach_point(s)
                if p:
                    pts.append(p[:2])
            placed = [(x, y) for t, x, y in m.team_objs.get(T.bare(team), []) if x >= 0 and y >= 0]
            if placed:
                pts.append((sum(p[0] for p in placed) / len(placed), sum(p[1] for p in placed) / len(placed)))
            else:
                pts += m.team_spawn.get(T.bare(team), [])[:1]
        out = []
        for p in pts:
            if all((p[0] - q[0]) ** 2 + (p[1] - q[1]) ** 2 > 150 ** 2 for q in out):
                out.append(p)
        return out

    def counter_acts(self, chain):
        """Counters in the chain fed by teams' on-destroyed scripts: (words, depth, go points)."""
        m = self.m
        out = []
        for t, d in chain:
            for (name, st, args), inv in zip(t['conditions'], t['not']):
                if inv or not name.startswith('COUNTER') or name == 'COUNTER_SECONDS':
                    continue
                counter = T.bare(next((a[3] for a in args if a[3]), ''))
                cmp_ = next((a[1] for a in args if a[0] == 6), 3)
                value = next((a[1] for a in args if a[0] == 0), 0)
                need = value + 1 if cmp_ == 4 else value
                if cmp_ not in (2, 3, 4) or need <= 0:
                    continue
                teams = []
                for inc in m.setters.get(('counter', counter), []):
                    teams += [x for x in m.on_destroyed.get(inc['name'], []) if x not in teams]
                # More needed than teams feed it: the counter counts something else as well (Lothlorien's
                # goblin caves wait for 50 with a handful of teams), so the team count would mislead.
                if not teams or need > len(teams):
                    continue
                pts = self.team_points(teams[:need])
                out.append((f'разбить {need} {teams_word(need)} врагов', d, pts))
        return out

    def lever_acts(self, chain):
        """GATE_IS_OPEN of an object the player opens himself. Grey Company's barrow gates and the seven
        grates of the Paths of the Dead wait for a DwarfWaystone, which map.ini makes a selectable
        structure with CastleGateCommandSet and GateOpenAndCloseBehavior; a script hands it to the
        player (NAMED_TRANSFER_OWNERSHIP_PLAYER), who opens it with its own button (read 2026-11-15)."""
        m = self.m
        out = []
        for t, d in chain:
            for (name, st, args), inv in zip(t['conditions'], t['not']):
                if inv or name != 'GATE_IS_OPEN' or not args:
                    continue
                obj = args[0][3]
                spot = m.named.get(obj)
                if not spot:
                    continue
                if 'WAYSTONE' in spot[0].upper():
                    words = 'открыть ворота рычагом: подведите отряд вплотную к этой точке, рычаг мод потянет сам'
                else:
                    words = 'открыть ворота: ' + self.names.obj(spot[0])
                out.append((words, d, [spot[2:4]]))
        return out

    def health_acts(self, chain):
        """UNIT_HEALTH of a named object below a few percent: Pelargir's inner barricade opens when
        "Pelargir - Besiege - Barricade" is beaten down to 3 % (comparison 0 <, 1 <=)."""
        m = self.m
        out = []
        for t, d in chain:
            for (name, st, args), inv in zip(t['conditions'], t['not']):
                if inv or name != 'UNIT_HEALTH' or not args:
                    continue
                cmp_ = next((a[1] for a in args if a[0] == 6), -1)
                value = next((a[1] for a in args if a[0] == 0), 100)
                obj = args[0][3]
                if cmp_ not in (0, 1) or value > 25 or obj not in m.named or m.side_of(obj) == 'player':
                    continue
                # A boss beaten down to a few percent (the Witch-king, the King of the Dead) is a fight
                # whose end a cinematic takes over; a barricade is broken.
                verb = 'разрушить: ' if G.is_building(self.names, m.named[obj][0]) else 'победить в бою: '
                out.append((verb + G.spoken(obj), d, [m.named[obj][2:4]]))
        return out

    def act(self, s, area):
        """What the player must do so that script s runs: (words, depth, go points), or None."""
        m = self.m
        chain = m.trace([s], set())
        poly = m.polys.get(area, [])
        best = None
        self.circular = False
        home = top(s)
        for words, d, pts in self.counter_acts(chain) + self.lever_acts(chain) + self.health_acts(chain):
            if best is None or d < best[1]:
                best = (words, d, [p for p in pts if p[0] >= 0 and p[1] >= 0])
        for n in sorted(m.needs(chain), key=lambda v: v['depth']):
            # "Dead" in a script's name marks a loss check, unless what died is the enemy: Pelennor's
            # "Pelennor - Siege - Dead Catapults" (TEAM_DESTROYED Siege - Catapults) is the goal.
            if n['depth'] > 8 or (n['bad'] and not (n['kind'] in ('kill', 'destroy') and n.get('side') != 'player')):
                continue
            via = top(m.by_name.get(n['via'], s))
            # Two steps back stay in the story even across groups (Minas Tirith's retreat waits for the
            # gate fight of group "Gate"); further back they ran from Gladden to Rivendell.
            # Fights only: a walk of another group ran from Gollum's beach to Frodo in Rivendell.
            if home not in GENERIC and via not in GENERIC and via != home and \
                    (n['depth'] > 2 or n['kind'] not in ('kill', 'destroy')):
                continue
            k, side = n['kind'], n.get('side')
            words, pts = '', []
            if k == 'crate':
                local = G.display(m, n['what']).replace('предмет: ', '')
                words = 'подобрать: ' + (local or ('след' if 'Footprints' in n['what'] else self.names.obj(n['what'])))
                pts = self.by_tmpl.get(n['what'].upper(), [])[:1]
            elif k == 'reach' and n.get('type') and n['cond'].startswith('PLAYER_HAS') and side != 'enemy':
                what = self.names.obj(n['type'])
                if gone(chain, n):
                    if 'Webbed' not in n['type']:  # "< 1 of the heroes" is a check for losing them
                        continue
                    words = 'освободить: ' + what
                else:
                    words = f"привести {what} в зону «{G.spoken(n['what'])}»"
                    pts = [(n['x'], n['y'])]
            elif k == 'reach' and side == 'player':
                who = n['who'] if n['who'] and n['who'] != m.me and not n['who'].startswith(('<', 'Plyr')) and '/' not in n['who'] else ''
                words = f"дойти до места «{G.spoken(n['what'])}»" + (f' (герой {who})' if who else '')
                pts = [(n['x'], n['y'])]
            elif k == 'destroy' and side != 'player':
                words = 'уничтожить: ' + G.spoken(n['what'])
                pts = [(n['x'], n['y'])] if n.get('x') else \
                    [m.created[n['what']][1:3]] if n['what'] in m.created else []
            elif k == 'kill' and side == 'enemy':
                words = 'разбить: ' + G.spoken(n['what'])
                if n['cond'] in ('TEAM_ON_DESTROYED', 'TEAM_DESTROYED'):
                    pts = self.team_points([n['what']])
                elif n['what'] in m.named:
                    pts = [m.named[n['what']][2:4]]
                elif n['what'] in m.created:
                    pts = [m.created[n['what']][1:3]]
            elif k == 'capture':
                words = 'захватить: ' + G.spoken(n['what'])
                pts = [(n['x'], n['y'])] if n.get('x') else []
            elif k == 'near':
                others = [u for u, sd in zip(n['what'].split(' + '), n.get('sides', [])) if sd != 'player']
                if others and all(sd != 'enemy' for sd in n.get('sides', [])):
                    words = 'подойти к: ' + G.spoken(others[0])
                    if others[0] in m.named:
                        pts = [m.named[others[0]][2:4]]
            # A walk into the area this script opens cannot come first (Westfold's captain cinematic).
            # Striking at what stands inside it can: Cirith Ungol's webs fill their own areas.
            if words and poly and k in ('reach', 'near') and any(inside(x, y, poly) for x, y in pts):
                self.circular |= n['depth'] == 0
                continue
            if words and (best is None or n['depth'] < best[1]):
                best = (words, n['depth'], [p for p in pts if p[0] >= 0 and p[1] >= 0])
        return best


def gone(chain, n):
    for t, d in chain:
        if t['name'] != n['via']:
            continue
        for name, st, args in t['conditions']:
            if name == n['cond'] and any(a[3] == n['type'] for a in args):
                return next((a[1] for a in args if a[0] == 6), 3) in (0, 1)
    return False


def read_guide_places(data_dir):
    """tag (upper, with PRE: if any) -> [(x, y)] from the hand and generated guides."""
    out = {}
    for fn in ('guides.ini', 'guides-auto.ini', 'stealth.ini'):
        path = os.path.join(data_dir, fn)
        if not os.path.exists(path):
            continue
        tag = None
        for line in open(path, encoding='utf-8-sig'):
            line = line.strip()
            if line.startswith('[') and line.endswith(']'):
                tag = line[1:-1].strip().upper()
                continue
            mm = re.match(r'(place|visit|step)\s*=\s*[^|]*\|\s*(-?[\d.]+)\s+(-?[\d.]+)', line)
            if tag and mm:
                out.setdefault(tag, []).append((float(mm.group(2)), float(mm.group(3))))
    return out


def start_points(mp):
    """Where the player's units stand at the start: the units placed on the player's side, and the
    start waypoint. Half the maps put "Player_1_Start" off the map (Cirith Ungol (-150, 262): the
    first flood reached 0 cells), so the units come first; a flood from all of them at once."""
    m = mp.m
    pts = list(mp.own)
    for key in (m.me + '_Start', 'Player_1_Start'):
        if key in m.wps:
            pts.append(m.wps[key])
    pts = [p for p in pts if p[0] >= 0 and p[1] >= 0]
    # Heroes a script hands over from a civilian team (Grey Company, Pelargir), then the first
    # guide place of the first objective.
    if not pts:
        pts = [m.named[u][2:4] for u in m.given if u in m.named and m.named[u][2] >= 0 and m.named[u][3] >= 0]
    if not pts and mp.first_place:
        pts = [mp.first_place]
    return pts


def reach_records(mp, gates, start_shut, places, rep):
    """[(x, y, need, any)] for places some gate cuts off."""
    m = mp.m
    raw, pos, nm = mp.raw
    grid = passmap.Grid(MAPPATH[m.folder], AOTR[0], raw, pos, nm)
    sp = start_points(mp)
    if not sp:
        rep.write('  reach: no start point\n')
        return []
    start = [grid.free_near(*p) for p in sp]
    sp = f'{len(sp)} points, first ({sp[0][0]:.0f}, {sp[0][1]:.0f})'
    cells = {a: grid.cells_in(m.polys[a]) for a in gates}

    def shut_of(areas):
        b = bytearray(grid.w * grid.h)
        for a in areas:
            for c in cells[a]:
                b[c] = 1
        return b

    def at(seen, x, y, sh=None):
        c = grid.free_near(x, y, sh)
        return c >= 0 and seen[c]

    open_all = grid.flood(start)
    # A start on a ledge the cells call closed (Beacons 3 cells, Khazad-dum 0): the first objective's
    # places are where the player goes next, so the flood starts there as well.
    if sum(open_all) < grid.w * grid.h // 50 and mp.first_places:
        start += [grid.free_near(*p) for p in mp.first_places]
        sp += f', plus {len(mp.first_places)} places of the first objective'
        open_all = grid.flood(start)
    # A gate the player's units start inside cuts nothing off for them: a flood from its blocked cells
    # would reach nothing and every place would "need" it.
    cellset = {a: set(cells[a]) for a in gates}
    gates = [a for a in gates if not any(c in cellset[a] for c in start)]
    start_shut = [a for a in start_shut if a in gates]
    single = {a: (grid.flood(start, shut_of([a])), shut_of([a])) for a in gates}
    sshut = shut_of(start_shut)
    all_start = grid.flood(start, sshut) if start_shut else None
    minus = {}
    out = []
    for x, y in places:
        if not at(open_all, x, y):
            continue
        need = [a for a in gates if not at(single[a][0], x, y, single[a][1])]
        anyl = []
        if not need and all_start is not None and not at(all_start, x, y, sshut):
            for a in start_shut:
                if a not in minus:
                    rest = [b for b in start_shut if b != a]
                    minus[a] = (grid.flood(start, shut_of(rest)), shut_of(rest))
                if at(minus[a][0], x, y, minus[a][1]):
                    anyl.append(a)
        if need or anyl:
            out.append((x, y, need, anyl))
    rep.write(f'  reach: start {sp}, {sum(open_all)} cells open, {len(out)} of {len(places)} places cut off by a gate\n')
    return out


MAPPATH, AOTR = {}, ['']


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    aotr, out_path, report = args[0], args[1], args[2]
    flt = args[3].lower() if len(args) > 3 else ''
    AOTR[0] = aotr
    do_reach = '--no-reach' not in sys.argv
    data_dir = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'data')
    gplaces = read_guide_places(data_dir)
    lotr = read_str(os.path.join(aotr, 'data', 'lotr.str'))
    names = G.Names(aotr, lotr)
    out = open(out_path, 'w', encoding='utf-8')
    rep = open(report, 'w', encoding='utf-8')
    out.write('; Generated by tools\\make_gates.py from the maps\' scripts. Do not edit by hand.\n')
    total = {'maps': 0, 'gates': 0, 'known': 0, 'with place': 0, 'cut places': 0}
    for folder in T.campaign_maps(aotr):
        if flt and flt not in folder.lower():
            continue
        MAPPATH[folder] = os.path.join(aotr, 'maps', folder, folder + '.map')
        try:
            mp = Map(aotr, folder, lotr, names)
        except Exception as e:  # noqa: BLE001
            rep.write(f'{folder}: unreadable ({e})\n')
            continue
        m = mp.m
        openers, closers = {}, {}
        for s in m.scripts:
            if SKIP.search(s['name'] + ' ' + s['group']):
                continue
            for a in s['actions']:
                if area_of(a) and shuts(a, False):
                    openers.setdefault(area_of(a), []).append(s)
                elif area_of(a) and shuts(a, True):
                    closers.setdefault(area_of(a), []).append(s)
        gates = [a for a in sorted(openers) if m.polys.get(a)]
        if not gates:
            continue
        # Shut from the first frame: a setup script with no real condition.
        start_shut = [a for a in gates if any(all(c[0] == 'CONDITION_TRUE' for c in s['conditions'])
                                              for s in closers.get(a, []))]
        total['maps'] += 1
        gate_go = {}
        out.write(f'\n[{folder}]\n')
        rep.write(f'\n=== {folder} ({len(gates)} gates, {len(start_shut)} shut at the start)\n')
        for area in gates:
            pts = m.polys[area]
            total['gates'] += 1
            cands = []
            for s in openers[area]:
                a = mp.act(s, area)
                # A script that itself waits for a walk into the shut area fires only after another opened it.
                cands.append((a is None, mp.circular, len(m.trace([s], set())), a[1] if a else 99, s['name'], a))
            cands.sort(key=lambda c: c[:4])
            chosen = cands[0][5]
            words = HAND[area] if area in HAND else (chosen[0] if chosen else '')
            go = chosen[2] if chosen else []
            xs, ys = [p[0] for p in pts], [p[1] for p in pts]
            total['known'] += bool(words)
            total['with place'] += bool(go)
            gos = '; '.join(f'{x:.0f} {y:.0f}' for x, y in go)
            poly = '; '.join(f'{p[0]:.0f} {p[1]:.0f}' for p in pts)
            out.write(f'{area} | {min(xs):.0f} {min(ys):.0f} {max(xs):.0f} {max(ys):.0f} | {words} | go {gos} | poly {poly}\n')
            when = 'start' if area in start_shut else ('later: ' + ', '.join(s['name'] for s in closers.get(area, [])[:2]) if area in closers else 'never shut by a script?')
            rep.write(f"  {area} [{when}]: {words or '?'}{'  (HAND)' if area in HAND else ''}\n      go {gos or '-'}\n")
            for nope, circ, size, depth, name, a in cands:
                rep.write(f"      {'*' if a is chosen else ' '} chain {size:3d}{' circular' if circ else ''} {name}: "
                          f"{a[0] + ' (depth %d)' % a[1] if a else '-'}\n")
            gate_go[area] = go
        if do_reach:
            places = []
            for t in m.tags:
                places += gplaces.get(t.upper(), []) + gplaces.get('PRE:' + t.upper(), [])
            for pts in gate_go.values():  # the mod also asks whether the way to an opening place is shut
                places += pts
            mp.first_places = next((gplaces[t.upper()] for t in m.tags if gplaces.get(t.upper())), [])
            mp.first_place = mp.first_places[0] if mp.first_places else None
            seen, uniq = set(), []
            for p in places:
                k = (round(p[0]), round(p[1]))
                if k not in seen:
                    seen.add(k)
                    uniq.append(p)
            try:
                # Only areas some script shuts: Westfold lifts 15 never-shut areas once more at the captain.
                recs = reach_records(mp, [a for a in gates if a in closers], start_shut, uniq, rep)
            except Exception as e:  # noqa: BLE001
                rep.write(f'  reach: failed ({e})\n')
                recs = []
            for x, y, need, anyl in recs:
                total['cut places'] += 1
                out.write(f"@ {x:.0f} {y:.0f} | need {'; '.join(need)} | any {'; '.join(anyl)}\n")
                rep.write(f"    @ ({x:.0f}, {y:.0f}) need {need} any {anyl}\n")
        out.flush()
        rep.flush()
        print(folder, flush=True)
    out.close()
    rep.close()
    print(total)


if __name__ == '__main__':
    main()
