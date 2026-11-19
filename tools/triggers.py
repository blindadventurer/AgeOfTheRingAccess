"""What moves every campaign mission on: per objective, the script conditions the player must
meet, traced through flags, timers, counters and script enabling.

Usage: python -I triggers.py <aotr folder> <report.txt> [map filter]

objective_places.py walks 4 steps back from a completing script and follows every setter, also
"set flag to false" and disabled scripts. Here:
  - only true flags, started timers, counters and ENABLE_SCRIPT / CALL_SUBROUTINE count as edges;
  - the walk has no fixed depth (12 at most) but stops at scripts that complete or show another
    objective, so a place belongs to the stretch of story of its own objective;
  - each objective also gets its "lead-in": what must happen before it is shown;
  - units are sorted into the player's, enemies and bystanders from the teams they start on.

model(aotr, folder) is used by make_guides.py.
"""
import os
import re
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mapfile  # noqa: E402
from missions_audit import read_str  # noqa: E402
from objective_places import inside  # noqa: E402

A_UNIT, A_PLAYER, A_TEAM, A_AREA, A_TYPE, A_POWER, A_SCIENCE, A_STATUS, A_TYPE2, A_INT = 14, 11, 3, 9, 61, 31, 32, 41, 15, 0
ENEMY = re.compile(r'^(PlyrCreeps|PlyrEvil|PlyrMordor|PlyrIsengard|PlyrGoblin|Player_[2-9])', re.I)
# Not "audio": talks run through "Audio - ..." scripts that start the next line's timer.
SKIP = re.compile(r'debug|\btest\b|music|\banim\b|idle|ambien|\bsfx\b|camera shake', re.I)
BAD = re.compile(r'\blose\b|\bloss\b|\bfail|defeat|\breset\b|\bdeath\b|\bdies\b|\bdead\b', re.I)
CAUGHT = re.compile(r'spotted|detect|caught|discover|noticed|sighted|\bseen\b|alert|\bspot\b|found you', re.I)
REACH = {'NAMED_INSIDE_AREA', 'NAMED_ENTERED_AREA', 'SKIRMISH_PLAYER_HAS_UNITS_IN_AREA', 'TEAM_INSIDE_AREA_PARTIALLY',
         'TEAM_INSIDE_AREA_ENTIRELY', 'TEAM_ENTERED_AREA_PARTIALLY', 'TEAM_ENTERED_AREA_ENTIRELY',
         'PLAYER_HAS_COMPARISON_UNIT_TYPE_IN_TRIGGER_AREA', 'PLAYER_HAS_COMPARISON_UNIT_KIND_IN_TRIGGER_AREA',
         'PLAYER_HAS_COMPARISON_UNIT_TYPE_IN_TRIGGER_AREA_WITH_UPGRADE', 'CAMERA_ENTERED_AREA'}
LEAVE = {'NAMED_OUTSIDE_AREA', 'NAMED_EXITED_AREA', 'SKIRMISH_PLAYER_IS_OUTSIDE_AREA', 'TEAM_OUTSIDE_AREA_ENTIRELY'}
NEAR = {'DISTANCE_BETWEEN_OBJ', 'DISTANCE_BETWEEN_TEAM'}
PICKUP = {'NAMED_DESTROYED', 'NAMED_NOT_DESTROYED'}
SIGHT = {'ENEMY_SIGHTED_BY_TEAM', 'ENEMY_SIGHTED', 'TYPE_SIGHTED', 'TEAM_DISCOVERED', 'NAMED_DISCOVERED',
         'SKIRMISH_PLAYER_HAS_DISCOVERED_PLAYER', 'TEAM_THREAT_LEVEL', 'UNIT_THREAT_LEVEL'}


# A NOT condition read as its opposite: "NOT NAMED_NOT_DESTROYED(Shield)" is the shield taken, "NOT
# TEAM_HAS_UNITS(Clue - Belt)" the belt picked up, "NOT ... HAS_UNITS_IN_AREA" an area left or cleared.
# Other NOT conditions are guards ("NOT attacking", "NOT INSIDE_GARRISON") and give no act (2026-11-08).
INVERSE = {'NAMED_NOT_DESTROYED': 'NAMED_DESTROYED', 'NAMED_DESTROYED': 'NAMED_NOT_DESTROYED',
           'TEAM_HAS_UNITS': 'TEAM_DESTROYED', 'SKIRMISH_PLAYER_HAS_UNITS_IN_AREA': 'SKIRMISH_PLAYER_IS_OUTSIDE_AREA',
           'TEAM_INSIDE_AREA_PARTIALLY': 'TEAM_OUTSIDE_AREA_ENTIRELY', 'NAMED_INSIDE_AREA': 'NAMED_OUTSIDE_AREA'}


def arg(args, kind, default=''):
    for t, i, f, s in args:
        if t == kind:
            return s if kind != A_INT else i
    return default


def read_pickups(text):
    """Pick-ups an INI text defines: template (upper) -> (template, DisplayName key)."""
    out = {}
    for m in re.finditer(r'^\s*(?:Object|ChildObject)\s+(\S+)(.*?)(?=^\s*(?:Object|ChildObject)\s+\S+|\Z)', text, re.M | re.S):
        if re.search(r'^\s*KindOf\s*=.*\bCRATE\b', m.group(2), re.M):
            name = re.search(r'^\s*DisplayName\s*=\s*(\S+)', m.group(2), re.M)
            out[m.group(1).upper()] = (m.group(1), name.group(1) if name else '')
    return out


GLOBAL_PICKUPS = {}


def bare(name):
    """State is often named with its side: "PlyrCivilian/Objective 04X" sets flag "Objective 04X"."""
    return name.split('/')[-1]


def sets(action):
    """State tokens an action turns on: ('flag'|'timer'|'counter'|'script', name)."""
    name, strings, args = action
    first = bare(next((a[3] for a in args if a[3]), ''))
    if name == 'SET_FLAG':
        return [('flag', first)] if len(args) > 1 and args[1][1] else []
    if 'TIMER' in name and name.startswith(('SET_', 'ADD_', 'RESTART')):
        return [('timer', first)]
    if 'COUNTER' in name and name.startswith(('SET_', 'INCREMENT', 'ADD_')):
        return [('counter', first)]
    if name in ('ENABLE_SCRIPT', 'CALL_SUBROUTINE'):
        return [('script', first)]
    if name in ('ENABLE_SCRIPT_GROUP', 'ENABLE_GROUP'):
        return [('group', first)]
    return []


def waits(cond):
    """State tokens a condition needs."""
    name, strings, args = cond
    first = bare(next((a[3] for a in args if a[3]), ''))
    if name == 'FLAG':
        return [('flag', first)] if len(args) > 1 and args[1][1] else []
    if name in ('TIMER_EXPIRED', 'TIMER_STARTED'):
        return [('timer', first)]
    if name.startswith('COUNTER'):
        return [('counter', first)]
    return []


class Mission:
    def __init__(self, aotr, folder, lotr):
        mdir = os.path.join(aotr, 'maps', folder)
        mpath = os.path.join(mdir, folder + '.map')
        text = open(os.path.join(mdir, 'map.ini'), 'rb').read().decode('latin-1')
        self.folder = folder
        if not GLOBAL_PICKUPS:
            GLOBAL_PICKUPS.update(read_pickups(open(os.path.join(aotr, 'data', 'ini', 'crate.ini'), 'rb').read().decode('latin-1')))
        self.pickups = dict(GLOBAL_PICKUPS)
        self.local_pickups = read_pickups(text)  # map.ini: AotRGCBooks, AotRFootprintsTroll...
        self.pickups.update(self.local_pickups)
        self.strs = dict(lotr)
        self.strs.update(read_str(os.path.join(mdir, 'map.str')))
        self.tags = [t for t in re.findall(r'(?:Bonus)?MissionObjectiveTag\s*=\s*(\S+)', text)]
        raw, pos, names = mapfile.load(mpath)
        self.scripts = list(mapfile.scripts(raw, pos, names))
        self.wps = mapfile.waypoints(raw, pos, names)
        self.polys = {t: pts for t, _, pts in mapfile.triggers(raw, pos, names)}
        self.named, self.paths = {}, []
        self.team_owner = mapfile.teams(raw, pos, names)
        self.team_objs = {}  # team (no side prefix) -> [(template, x, y)] placed on the map
        self.size = (1.0, 1.0)
        for tmpl, props, (x, y, z) in mapfile.objects(raw, pos, names):
            self.size = (max(self.size[0], x), max(self.size[1], y))
            for k in ('waypointPathLabel1', 'waypointPathLabel2', 'waypointPathLabel3'):
                if props.get(k):
                    self.paths.append((props[k], x, y))
            if props.get('objectName'):
                self.named[props['objectName']] = (tmpl, props.get('originalOwner', ''), x, y)
            if '/' in props.get('originalOwner', ''):
                self.team_objs.setdefault(bare(props['originalOwner']), []).append((tmpl, x, y))
        self.areas = {}
        for t, pts in self.polys.items():
            if pts:
                cx, cy = sum(p[0] for p in pts) / len(pts), sum(p[1] for p in pts) / len(pts)
                wp = [w for w in self.wps.values() if inside(w[0], w[1], pts)]
                self.areas[t] = min(wp, key=lambda w: (w[0] - cx) ** 2 + (w[1] - cy) ** 2) if wp else (cx, cy)
        self.teams = {}  # named unit -> team it is created on or moved to
        for s in self.scripts:
            for a, st, args in s['actions']:
                if a.startswith(('CREATE_NAMED_ON_TEAM', 'UNIT_SET_TEAM', 'CREATE_NAMED')) and arg(args, A_TEAM):
                    unit = arg(args, A_UNIT) or (args[0][3] if args else '')
                    self.teams.setdefault(unit, arg(args, A_TEAM))
        self.created = {}  # named unit -> (template, x, y) of the script that creates it
        self.lists = {}  # object list a script fills -> templates ("player wild LIST" in Rivendell)
        for s in self.scripts:
            for a, st, args in s['actions']:
                if a.startswith('CREATE_NAMED') and len(args) >= 2:
                    w = self.wps.get(args[-1][3], (0, 0))
                    self.created.setdefault(args[0][3], (args[1][3], w[0], w[1]))
                elif a == 'OBJECTLIST_ADDOBJECTTYPE' and len(args) >= 2:
                    self.lists.setdefault(args[0][3], []).append(args[1][3])
        # Units a script brings in later (UNIT_SPAWN_NAMED_LOCATION_ORIENTATION: name, template, location,
        # angle): Taming of Smeagol's barrow lords exist only after their cinematics (2026-11-08).
        for s in self.scripts:
            for a, st, args in s['actions']:
                if a == 'UNIT_SPAWN_NAMED_LOCATION_ORIENTATION' and len(args) >= 2 and args[0][3]:
                    w = self.wps.get(args[2][3], (0, 0)) if len(args) > 2 else (0, 0)
                    self.created.setdefault(args[0][3], (args[1][3], w[0], w[1]))
        # Teams whose end a script counts: the Teams chunk names an on-destroyed script per team
        # ("PS Night Camp" of Ugluk and the six captor teams in Fangorn), which then raises a counter
        # an objective waits for. Without this, "destroy these teams" objectives had no target.
        self.on_destroyed = {}  # script name -> [team]
        for name, ver, start, end, _ in mapfile.walk(raw, pos, names):
            if name == 'Teams':
                r = mapfile.Reader(raw, start, end, names)
                for _ in range(r.u32()):
                    d = r.dict()
                    sc = d.get('teamOnDestroyedScript')
                    if sc and d.get('teamName'):
                        self.on_destroyed.setdefault(sc, []).append(d['teamName'])
        # Where a team's units come from when none are placed: the waypoints of the scripts that
        # create them (CREATE_*_ON_TEAM_AT_WAYPOINT, CREATE_REINFORCEMENT_TEAM).
        self.team_spawn = {}  # team (no side prefix) -> [(x, y)]
        for s in self.scripts:
            for a, st, args in s['actions']:
                ss = [x[3] for x in args if x[3]]
                team = wp = ''
                if a in ('CREATE_UNNAMED_ON_TEAM_AT_WAYPOINT',) and len(ss) >= 3:
                    team, wp = ss[1], ss[2]
                elif a == 'CREATE_NAMED_ON_TEAM_AT_WAYPOINT' and len(ss) >= 4:
                    team, wp = ss[2], ss[3]
                elif a == 'CREATE_REINFORCEMENT_TEAM' and len(ss) >= 2:
                    team, wp = ss[0], ss[1]
                if team and wp in self.wps:
                    self.team_spawn.setdefault(bare(team), []).append(self.wps[wp])
        self.me = self.human()
        # Heroes wait on a civilian team until a script hands them over (Paths of the Dead: Aragorn,
        # Grey Company: Farmer Maggot's cart): they are the player's, whatever team they start on.
        self.given = set()
        for s in self.scripts:
            for a, st, args in s['actions']:
                if a == 'UNIT_SET_TEAM' and arg(args, A_TEAM).startswith(self.me) and arg(args, A_UNIT):
                    self.given.add(arg(args, A_UNIT))
        self.setters, self.by_name, self.groups = {}, {}, {}
        for s in self.scripts:
            self.by_name.setdefault(s['name'], s)
            self.groups.setdefault(s['group'], []).append(s)
            for a in s['actions'] + s['false']:
                for tok in sets(a):
                    self.setters.setdefault(tok, []).append(s)
        self.done, self.show = {}, {}
        for s in self.scripts:
            for a, st, args in s['actions']:
                n = args[0][1] if args else 0
                if a == 'MARK_MISSION_OBJECTIVE_COMPLETED':
                    self.done.setdefault(n - 1, []).append(s)
                elif a == 'SHOW_MISSION_OBJECTIVE':
                    self.show.setdefault(n - 1, []).append(s)
        # Bonus objectives with no MARK: the "completed" box names the tag.
        for i, tag in enumerate(self.tags):
            if i not in self.done:
                lst = [s for s in self.scripts for a, st, args in s['actions']
                       if a.startswith('DISPLAY_NOTIFICATION_BOX') and tag in st and 'Completed' in ' '.join(st)]
                if lst:
                    self.done[i] = lst

    def human(self):
        """The player's side. Story maps use Player_1; the War of the Ring missions (map sp good,
        map sp evil, map ang) keep Player_1 almost empty and script Player_2 instead, so the side
        the scripts name most is the player's (Rivendell 57 to 3, Balin's Expedition 154 to 0)."""
        refs = Counter()
        for s in self.scripts:
            for c in s['conditions'] + s['actions'] + s['false']:
                for t, i, f, v in c[2]:
                    if t == A_PLAYER and v in ('Player_1', 'Player_2'):
                        refs[v] += 1
        return 'Player_2' if refs['Player_2'] > refs['Player_1'] else 'Player_1'

    def is_enemy(self, name):
        return not name.startswith(self.me) and bool(ENEMY.match(name))

    def pickup(self, args):
        """The pick-up a condition names: by template, object list or named object. Scripts test
        them from any side and any distance: "PlyrCivilian has fewer than 1 AthelasCrate in Area -
        Forest - Athelas 01" means the player took it, NAMED_NOT_DESTROYED the same."""
        for t, i, f, v in args:
            if not v:
                continue
            for name in self.lists.get(v, [v]):
                tmpl = name if name.upper() in self.pickups else self.named.get(name, self.created.get(name, ('',)))[0]
                if tmpl and tmpl.upper() in self.pickups:
                    return self.pickups[tmpl.upper()][0]
            # TEAM_HAS_UNITS "PlyrCivilian/Clue - Farm" turned round: the team's one crate is gone.
            if t == A_TEAM:
                for tmpl, x, y in self.team_objs.get(bare(v), []):
                    if tmpl.upper() in self.pickups:
                        return self.pickups[tmpl.upper()][0]
        return ''

    def side_of(self, unit):
        if unit in self.given:
            return 'player'
        team = self.teams.get(unit) or self.named.get(unit, ('', ''))[1]
        if not team:
            return '?'
        if '/' not in team:  # "Arwen TEAM": the Teams chunk names its side
            team = self.team_owner.get(team, team)
        if team.startswith(self.me):
            return 'player'
        return 'enemy' if self.is_enemy(team) else 'bystander'

    def text(self, tag):
        return re.sub(r'\s+', ' ', self.strs.get(tag.upper(), '?'))

    def before(self, s):
        """Scripts s waits for."""
        out = []
        for c in s['conditions']:
            for tok in waits(c):
                out += self.setters.get(tok, [])
        out += self.setters.get(('script', bare(s['name'])), [])
        # ENABLE_SCRIPT names groups too (Celduin "IID - Area 1 - Dwarven Base - Behavior"), and
        # "Setup" scripts with no condition wait for their group or a group above it.
        for part in s['group'].split('/'):
            out += self.setters.get(('script', part), [])
            out += self.setters.get(('group', part), [])
        return out

    def trace(self, start, stop):
        """[(script, depth)] walking back from start; scripts in stop end the walk there."""
        seen, chain, frontier = set(), [], [(s, 0) for s in start]
        while frontier:
            s, d = frontier.pop(0)
            if id(s) in seen:
                continue
            seen.add(id(s))
            where = s['name'] + ' ' + s['group']
            if d and SKIP.search(where):
                continue
            chain.append((s, d))
            # The script just before the completion may also start the next objective.
            if d >= 12 or (d >= 2 and id(s) in stop):
                continue
            frontier += [(t, d + 1) for t in self.before(s)]
        return chain

    def barrier(self, skip):
        """Scripts that complete or show an objective other than skip, and those setting their state."""
        stop = set()
        for table in (self.done, self.show):
            for i, lst in table.items():
                if i == skip:
                    continue
                for s in lst:
                    stop.add(id(s))
                    for t in self.before(s):
                        stop.add(id(t))
        return stop

    def needs(self, chain):
        """What the scripts of a chain test: dicts with kind, what, x, y, who, depth, via, bad."""
        out = []
        for s, d in chain:
            where = s['name'] + ' ' + s['group']
            # The completing script itself can say "Dead" ("Drogoth Dead") or sit in a group "Win-Loss".
            bad = d > 0 and bool(BAD.search(where))
            # An on-destroyed script of teams: their end is what the chain waits for.
            for team in self.on_destroyed.get(s['name'], []):
                out.append({'cond': 'TEAM_ON_DESTROYED', 'depth': d, 'via': s['name'], 'bad': False, 'who': team,
                            'kind': 'kill', 'what': team, 'side': 'enemy', 'x': 0, 'y': 0})
            # Inverted conditions ("NOT INSIDE_GARRISON", "NOT attacking") are guards, not acts of the
            # player: the reader missed the flag until 2026-11-08 and read them as goals.
            for (name, strings, args), inverted in zip(s['conditions'], s.get('not', [False] * len(s['conditions']))):
                if inverted:
                    if name not in INVERSE:
                        continue
                    name = INVERSE[name]
                area = next((a[3] for a in args if a[3] in self.polys), '')
                unit, team = arg(args, A_UNIT), arg(args, A_TEAM)
                player = arg(args, A_PLAYER)
                base = {'cond': name, 'depth': d, 'via': s['name'], 'bad': bad, 'who': unit or team or player}
                crate = self.pickup(args)
                if crate:
                    base.update(kind='crate', what=crate, side='player', x=0, y=0)
                    out.append(base)
                    continue
                # "This side has none of them left": comparison <= or == with 0 (Spider Lairs).
                kind_ = arg(args, A_TYPE) or arg(args, A_TYPE2)
                cmp_ = next((a[1] for a in args if a[0] == 6), -1)
                if name.startswith(('PLAYER_HAS_OBJECT_COMPARISON', 'PLAYER_HAS_COMPARISON_UNIT_TYPE_IN_TRIGGER_AREA')) \
                        and self.is_enemy(player) and kind_ and cmp_ in (1, 2) and arg(args, A_INT, 0) == 0:
                    base.update(kind='wipe', what=kind_, side='enemy', x=0, y=0)
                    out.append(base)
                elif name == 'PLAYER_HAS_COMPARISON_UNIT_TYPE_IN_TRIGGER_AREA_COMPLETELY_BUILT' and area in self.areas \
                        and not self.is_enemy(player):
                    pl = self.areas[area]
                    base.update(kind='buildat', what=area, x=pl[0], y=pl[1], side='player', type=kind_)
                    out.append(base)
                elif name in REACH or name in LEAVE:
                    pl = self.areas.get(area)
                    if not pl:
                        continue
                    side = self.side_of(unit) if unit else ('enemy' if self.is_enemy(team or player) else
                                                             'bystander' if (team or player).startswith('Plyr') else 'player')
                    base.update(kind='camera' if name.startswith('CAMERA') else 'leave' if name in LEAVE else 'reach',
                                what=area, x=pl[0], y=pl[1], side=side, type=arg(args, A_TYPE))
                    out.append(base)
                elif name in NEAR:
                    other = [a[3] for a in args if a[3] and a[3] != (unit or team) and a[0] in (A_UNIT, A_TEAM)]
                    units = [a[3] for a in args if a[3] and a[0] in (A_UNIT, A_TEAM)]
                    dist = next((a[2] for a in args if a[0] == 1), 0)
                    base.update(kind='near', what=' + '.join(units), dist=dist, side='?',
                                sides=[self.side_of(u) for u in units], x=0, y=0)
                    out.append(base)
                elif name in PICKUP and unit:
                    t = self.named.get(unit, ('', '', 0, 0))
                    base.update(kind='pickup' if name == 'NAMED_NOT_DESTROYED' else 'destroy', what=unit, tmpl=t[0],
                                x=t[2], y=t[3], side=self.side_of(unit))
                    out.append(base)
                elif 'SPECIAL_POWER' in name and arg(args, A_POWER):
                    base.update(kind='power', what=arg(args, A_POWER), side='player', x=0, y=0)
                    out.append(base)
                elif name == 'NAMED_OWNED_BY_PLAYER' and unit:
                    t = self.named.get(unit, ('', '', 0, 0))
                    base.update(kind='capture', what=unit, tmpl=t[0], x=t[2], y=t[3], side='player')
                    out.append(base)
                elif name == 'NAMED_SELECTED' and unit:
                    base.update(kind='select', what=unit, side='player', x=0, y=0)
                    out.append(base)
                elif name in SIGHT:
                    base.update(kind='sight', what=unit or team or player, side=self.side_of(unit), x=0, y=0)
                    out.append(base)
                elif name == 'UNIT_HAS_OBJECT_STATUS' and arg(args, A_STATUS) in ('HIDDEN', 'STEALTHED', 'DETECTED'):
                    base.update(kind='status', what=f'{unit} {arg(args, A_STATUS)}', side='player', x=0, y=0)
                    out.append(base)
                elif name in ('NAMED_DESTROYED', 'TEAM_DESTROYED', 'PLAYER_HAS_KILLED_TYPE_UNITS'):
                    base.update(kind='kill', what=unit or team, side='enemy', x=0, y=0)
                    out.append(base)
                elif name.startswith('PLAYER_HAS_OBJECT_COMPARISON') or 'COMPLETELY_BUILT' in name:
                    base.update(kind='build', what=arg(args, A_TYPE), side='player', x=0, y=0)
                    out.append(base)
        return out

    def objective(self, i):
        d = {'tag': self.tags[i], 'text': self.text(self.tags[i]), 'done': [s['name'] for s in self.done.get(i, [])]}
        stop = self.barrier(i)
        chain = self.trace(self.done.get(i, []), stop)
        d['goal'] = self.needs(chain)
        lead = self.trace(self.show.get(i, []), stop)
        d['lead'] = self.needs(lead)
        return d


def campaign_maps(aotr):
    root = os.path.join(aotr, 'maps')
    for folder in sorted(os.listdir(root)):
        ini, mp = os.path.join(root, folder, 'map.ini'), os.path.join(root, folder, folder + '.map')
        if os.path.exists(ini) and os.path.exists(mp) and b'MissionObjectiveList' in open(ini, 'rb').read():
            yield folder


def line(n):
    c = f" ({n['x']:.0f}, {n['y']:.0f})" if n.get('x') else ''
    extra = ''
    if n['kind'] == 'near':
        extra = f" dist {n['dist']:.0f} {n['sides']}"
    if n.get('type'):
        extra += f" type {n['type']}"
    return f"{n['kind']:7s} {n['what']}{c} [{n.get('side')}] who={n['who']} step {n['depth']} via {n['via']!r}{extra}"


def main():
    aotr, out_path = sys.argv[1], sys.argv[2]
    flt = sys.argv[3].lower() if len(sys.argv) > 3 else ''
    lotr = read_str(os.path.join(aotr, 'data', 'lotr.str'))
    out = open(out_path, 'w', encoding='utf-8')
    total = Counter()
    for folder in campaign_maps(aotr):
        if flt and flt not in folder.lower():
            continue
        try:
            m = Mission(aotr, folder, lotr)
        except Exception as e:  # noqa: BLE001
            out.write(f'\n=== {folder}: unreadable ({e})\n')
            continue
        out.write(f'\n=== {folder}: {len(m.scripts)} scripts, {len(m.areas)} areas, {len(m.tags)} objectives\n')
        for i in range(len(m.tags)):
            d = m.objective(i)
            total['objectives'] += 1
            kinds = Counter(n['kind'] for n in d['goal'] if not n['bad'])
            total['with place'] += any(n['kind'] in ('reach', 'pickup', 'near', 'camera', 'capture') and n['side'] != 'enemy' and not n['bad'] for n in d['goal'])
            out.write(f"  [{i}] {d['tag']}: {d['text']}\n      completed by {d['done'][:3]}; {dict(kinds)}\n")
            seen = set()
            for tag, lst in (('goal', d['goal']), ('lead', d['lead'])):
                for n in sorted(lst, key=lambda v: -v['depth']):
                    key = (tag, n['kind'], n['what'])
                    if key in seen or n['bad'] or n['side'] == 'enemy' or n['kind'] in ('kill', 'build'):
                        continue
                    seen.add(key)
                    out.write(f'      {tag}: {line(n)}\n')
        sys.stdout.flush()
    out.close()
    print(dict(total))


if __name__ == '__main__':
    main()
