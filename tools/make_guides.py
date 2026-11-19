"""Write data\\guides-auto.ini: for every campaign objective, the places its completing
scripts wait for the player's units (or a named hero) to reach.

Usage: python -I make_guides.py <aotr folder> <out.ini>

Kept: places found at most 2 steps back from the completing script (objective_places.py)
by a condition that tests the player's units or a named unit in an area, e.g.
NAMED_INSIDE_AREA Bilbo, SKIRMISH_PLAYER_HAS_UNITS_IN_AREA Player_1. Conditions about the
enemy (PlyrCreeps, other players' teams) are dropped: those are fights, not routes.
The mod reads this file after guides.ini; a hand-written guide for a tag wins.

autopath: a waypoint path of the map (waypointPathLabel1..3 of the waypoints in ObjectsList)
with a waypoint inside one of the objective's areas (or within 40 units of a waypoint place).
The mod walks it with the scripts' own "follow waypoints exactly" when ordinary move orders
stall: in Council of Elrond only the dwarves' path "Bilbo - Waypath Thorin" got Bilbo into
the end area (2026-11-01 07:19). Several paths: the one with a waypoint nearest the place.
"""
import glob
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mapfile  # noqa: E402
import objective_places as op  # noqa: E402
import triggers as T  # noqa: E402
from missions_audit import read_objects_ini, read_str, resolved, walk_ini  # noqa: E402

REACH = re.compile(r'INSIDE_AREA|ENTERED_AREA|UNITS_IN_AREA|IN_TRIGGER_AREA', re.I)
ENEMY = re.compile(r'^(PlyrCreeps|PlyrEvil|PlyrMordor|PlyrIsengard|PlyrGoblin|Player_[2-9])', re.I)
DEBUG = re.compile(r'debug|test', re.I)
NOISE = re.compile(r'impassable|blocker', re.I)
# A "whole map" area is noise as a step of a chain, but the very area an objective tests is real
# (evil Mirkwood: "Advance to the Old Forest Road" = "Area Trigger - Reveal Whole Map_1").
WIDE = re.compile(r'whole map|reveal all', re.I)
# Scripts about losing or resetting are not what completes an objective (Grey Company: a
# "Building Rubble in the farmstead" check reached every objective through shared flags).
NOT_GOAL = re.compile(r'lose|loss|fail|defeat|reset|death|dies|dead', re.I)

# Condition argument types (scripts of the 36 story maps, read 2026-11-01): 14 unit name,
# 11 player, 3 team, 9 trigger area, 61 object type or object list, 31 special power,
# 32 science, 41 object status, 15 object type, 6 comparison, 0 int, 1 real.
A_UNIT, A_PLAYER, A_TEAM, A_AREA, A_TYPE, A_POWER, A_SCIENCE, A_STATUS, A_TYPE2, A_INT = 14, 11, 3, 9, 61, 31, 32, 41, 15, 0


def arg(args, kind, default=''):
    for t, i, f, s in args:
        if t == kind:
            return s if kind != A_INT else i
    return default


class Names:
    """Readable names: lotr.str + map.str, object display names, command buttons by special
    power and by science (label and the hotkey letter after '&')."""

    def __init__(self, aotr, lotr):
        ini = os.path.join(aotr, 'data', 'ini')
        self.lotr = lotr
        # crate.ini lies outside the object folder: "Athelas Crate" would be its display name.
        self.objects = read_objects_ini(list(walk_ini(os.path.join(ini, 'object'))) + [os.path.join(ini, 'crate.ini')])
        self.global_buttons = self.read_buttons([os.path.join(ini, 'commandbutton.ini')] +
                                                glob.glob(os.path.join(ini, 'includes', '*button*.inc')))

    @staticmethod
    def read_buttons(paths):
        out = {}  # ('power' | 'science', name upper) -> text label key
        for p in paths:
            try:
                text = open(p, 'rb').read().decode('latin-1')
            except OSError:
                continue
            for m in re.finditer(r'^\s*CommandButton\s+(\S+)(.*?)^\s*End\b', text, re.M | re.S):
                body = m.group(2)
                label = re.search(r'^\s*TextLabel\s*=\s*(\S+)', body, re.M)
                if not label:
                    continue
                for kind, key in (('power', 'SpecialPower'), ('science', 'Science')):
                    v = re.search(r'^\s*' + key + r'\s*=\s*(\S+)', body, re.M)
                    if v:
                        out.setdefault((kind, v.group(1).upper()), label.group(1))
        return out

    def for_map(self, mdir, strs):
        self.strs = strs
        self.buttons = dict(self.global_buttons)
        self.buttons.update(self.read_buttons([os.path.join(mdir, 'map.ini')]))

    def button(self, kind, name):
        key = self.buttons.get((kind, name.upper()))
        text = self.strs.get(key.upper(), '') if key else ''
        hot = re.search(r'&(\w)', text)
        clean = re.sub(r'\s*\(&\w\)', '', text).replace('&', '').strip()
        return clean, hot.group(1).upper() if hot else ''

    def obj(self, template):
        if template.lower().startswith('object list'):
            # "Object Lists - HD Heroes": a list the map defines
            return 'отряды из списка «' + spoken(re.sub(r'(?i)^object lists?\s*-\s*', '', template)) + '»'
        key = resolved(self.objects, template, 'displayname')
        name = self.strs.get(key.upper(), '') if key else ''
        name = re.split(r'\s{2,}', name)[0].strip()  # "Building Rubble  Will rebuild...": the name only
        if name:
            return name
        return re.sub(r'(?<=[a-z])(?=[A-Z])', ' ', template).replace('_', ' ')


KEEP = re.compile(r'defend|protect|survive|hold|keep|find all', re.I)


def foe(m, unit, depth):
    """An object the objective wants destroyed. Bosses wait on a civilian team until the fight
    (Cirith Ungol: Gorbag, Fords of Isen: Mog), so a bystander counts when the completing script
    itself or the one before names it; further back it is mostly another story's business.
    A named object with no owner in the map file ("?": Fornost's Farm 01, Grey Havens' Tower01)
    is one too."""
    side = m.side_of(unit)
    return side == 'enemy' or (side in ('bystander', '?') and depth <= 1 and not m.keep)


def actions_for(m, chain, names, named, areas):
    """Russian phrases for what the player must do in the objective's script chain (up to two
    steps back), and extra places (areas the camera must look at)."""
    out, places = [], []

    def add(p, unit=None):
        if p and p not in out:
            out.append(p)
            # A named object's spot on the map at the start (buildings stay, units move on).
            if unit and unit in named and len(places) < 6:
                places.append((p, named[unit][2], named[unit][3], []))

    for s, depth in chain:
        if depth > 3 or DEBUG.search(s['name'] + ' ' + s['group']) or NOT_GOAL.search(s['name'] + ' ' + s['group']):
            continue
        teams = m.on_destroyed.get(s['name'], [])
        if teams and depth <= 2 and not m.keep:  # "Keep Grip, Fang and Wolf alive" shares the camp's chain
            add('разбить отряды: ' + ', '.join(spoken(t) for t in teams[:6]) + (f' и ещё {len(teams) - 6}' if len(teams) > 6 else ''))
        for (cname, strings, args), inverted in zip(s['conditions'], s.get('not', [False] * len(s['conditions']))):
            if inverted:  # its opposite (triggers.INVERSE), or a guard ("NOT attacking") with no act
                if cname not in T.INVERSE:
                    continue
                cname = T.INVERSE[cname]
            # Abilities are looked for up to 3 steps back (Lord of the Rings: the last answer
            # to Ar-Pharazon is 2 steps before "Objective 03 - Complete"); the rest 1 step,
            # deeper ones were mostly other objectives' business.
            if depth > 1 and 'SPECIAL_POWER' not in cname:
                continue
            # Comparison ints (Zero Hour): 0 <, 1 <=, 2 ==, 3 >=, 4 >, 5 !=; "fewer than" is a defence check.
            cmp_ = next((i for t, i, f, st in args if t == 6), 3)
            if cmp_ in (0, 1, 5) and 'COMPARISON' in cname:
                continue
            player = arg(args, A_PLAYER)
            ours = player in ('', m.me) or 'Local Player' in player
            unit = arg(args, A_UNIT)
            if 'SPECIAL_POWER' in cname and arg(args, A_POWER) and ours:
                label, hot = names.button('power', arg(args, A_POWER))
                if label:
                    add(f'способность «{label}»' + (f', клавиша {hot}' if hot else '') + (f', у {unit}' if unit else ''))
            elif cname == 'NAMED_DESTROYED' and unit and foe(m, unit, depth):
                add(f'уничтожить: {spoken(unit)}', unit)
            elif cname == 'TEAM_DESTROYED' and arg(args, A_TEAM):
                add(f'разбить отряд: {spoken(arg(args, A_TEAM))}')
            elif cname.startswith('PLAYER_HAS_COMPARISON_UNIT_TYPE_IN_TRIGGER_AREA') and ours and arg(args, A_AREA):
                what = names.obj(arg(args, A_TYPE))
                if 'COMPLETELY_BUILT' in cname:
                    add(f'построить {what} в зоне «{spoken(arg(args, A_AREA))}»')
                else:
                    add(f'привести {what} в зону «{spoken(arg(args, A_AREA))}»')
            elif cname == 'PLAYER_HAS_OBJECT_COMPARISON' and ours and arg(args, A_TYPE):
                add(f'нужно: {names.obj(arg(args, A_TYPE))}')
            elif cname == 'PLAYER_ACQUIRED_SCIENCE' and ours:
                label, _ = names.button('science', arg(args, A_SCIENCE))
                add(f'купить заклинание «{label or arg(args, A_SCIENCE)}»')
            elif cname == 'GATE_IS_OPEN' and unit:
                add(f'открыть ворота: {spoken(unit)}', unit)
            elif cname == 'NAMED_SELECTED' and unit:
                add(f'выделить: {spoken(unit)}', unit)
            elif cname == 'CAMERA_ENTERED_AREA' and arg(args, A_AREA):
                a = arg(args, A_AREA)
                add(f'навести камеру на «{spoken(a)}» (Ctrl+Home на это место)')
                if a in areas:
                    places.append((a, areas[a][0], areas[a][1], []))
            elif cname == 'UNIT_HAS_TOGGLED_WEAPON' and ours:
                add(f'переключить оружие: {names.obj(arg(args, A_TYPE2))}')
            elif cname == 'UNIT_HAS_OBJECT_STATUS' and unit and arg(args, A_STATUS) in ('HIDDEN', 'STEALTHED'):
                add(f'{unit} должен быть невидим')
            elif cname == 'NAMED_OWNED_BY_PLAYER' and unit and ours:
                add(f'захватить или получить: {spoken(unit)}', unit)
            elif cname == 'PLAYER_HAS_KILLED_TYPE_UNITS' and ours:
                n = arg(args, A_INT, 0)
                add(f'убить{" " + str(n) if n and n > 1 else ""}: {names.obj(arg(args, A_TYPE))}')
            elif cname in ('NAMED_DISCOVERED', 'TEAM_DISCOVERED') and ours:
                add(f'найти: {spoken(unit or arg(args, A_TEAM))}', unit)
            elif cname == 'BUILT_BY_PLAYER':
                add(f'построить или нанять: {names.obj(arg(args, A_TYPE))}')
            elif cname == 'NAMED_ATTACKED_BY_PLAYER' and unit:
                add(f'атаковать: {spoken(unit)}', unit)
            elif cname == 'IS_AREA_ON_FIRE' and arg(args, A_AREA):
                add(f'огонь в зоне «{spoken(arg(args, A_AREA))}»')
    return out, places


# Script writers' prefixes: acts ("IIIM", "ID", "IN"), "Area 2", "_Objectives", "Area Trigger".
SERVICE = re.compile(r'^(I{1,3}[A-Z]?|IN|_?objectives?|setup|area( trigger)?( \d+)?|trigger|_intro|_runner)$', re.I)


def spoken(area):
    # "Area - Mirkwood - Party - Feast" -> "Mirkwood, Party, Feast";
    # "IIM - Area 2 - Hobbiton" -> "Hobbiton"; "PlyrCreeps/Elves Team 1" -> "Elves Team 1"
    name = area.split('/')[-1]
    parts = []
    for p in name.split(' - '):
        p = re.sub(r'\s+AREA$|_\d+$', '', p.strip())  # "Rivendell AREA", "Whole Map_1"
        if p and not SERVICE.match(p):
            parts.append(p)
    # An area named for what it does to the map, not for a place in it.
    if not parts or re.fullmatch(r'(reveal\s+)?whole map', ' '.join(parts), re.I):
        return 'место задания'
    # "Entrance" alone says nothing when each area has one: keep its number.
    number = re.search(r'\bArea (\d+)', name)
    if len(parts) == 1 and number and parts[0].lower() in ('entrance', 'exit', 'midpoint', 'complete'):
        return f'{parts[0]}, участок {number.group(1)}'
    return ', '.join(parts)


def big(m, pts):
    """A map-wide area ("Whole Map") is no place to walk to. A large one still is: walking to
    its middle enters it (Ettenmoors "Area - Ettenmoors" for "Reach the elven settlement")."""
    xs, ys = [p[0] for p in pts], [p[1] for p in pts]
    return max(xs) - min(xs) > 0.7 * m.size[0] and max(ys) - min(ys) > 0.7 * m.size[1]


def spot(m, unit):
    """(template, x, y) of a named object: placed on the map or created by a script."""
    if unit in m.named:
        return m.named[unit][0], m.named[unit][2], m.named[unit][3]
    return m.created.get(unit, ('', 0, 0))


def is_building(names, template):
    return 'STRUCTURE' in resolved(names.objects, template, 'kindof').upper()


def targets(m, names, n, places, later, objects, seen):
    """What must be destroyed. Buildings stay where they stand, so they are places; units move,
    so they are objects with a live position."""
    k = n['kind']
    if n['depth'] > 3 or not n['what']:
        return
    if k == 'destroy' and foe(m, n['what'], n['depth']) and (n.get('side') == 'enemy' or not n['bad']):
        t, x, y = spot(m, n['what'])
        if is_building(names, t) and x and n['what'] not in seen:
            seen.add(n['what'])
            later.append(('уничтожить: ' + spoken(n['what']), x, y, n['via']))
        elif t and not is_building(names, t) and 'PARTICLESYSPROXY' not in t.upper() and t not in seen:
            seen.add(t)
            objects.append((t, ''))
    elif k == 'kill':
        team = n['what'].split('/')[-1]
        # Units parked off the map until a cinematic brings them (Fangorn's captors at x -1300) are no
        # place; then the waypoints the scripts create the team at.
        on_map = lambda p: 0 <= p[1] <= m.size[0] and 0 <= p[2] <= m.size[1]
        members = [p for p in m.team_objs.get(team, []) if on_map(p)]
        if not members:
            members = [('', x, y) for x, y in m.team_spawn.get(team, []) if on_map(('', x, y))]
        if members and team not in seen:
            seen.add(team)
            cx, cy = sum(p[1] for p in members) / len(members), sum(p[2] for p in members) / len(members)
            _, x, y = min(members, key=lambda p: (p[1] - cx) ** 2 + (p[2] - cy) ** 2)
            later.append(('разбить отряд: ' + spoken(team), x, y, n['via']))
    elif k == 'wipe':
        for t in sorted(m.lists.get(n['what'], [n['what']]), key=lambda v: not is_building(names, v)):
            if t not in seen:
                seen.add(t)
                objects.append((t, ''))


def collect(m, needs, names, goal=True):
    """Places and objects of a list of needs, the earliest in the story first. goal: the needs
    of the objective itself, which adds what it must destroy, clear or build."""
    places, later, objects, seen = [], [], [], set()
    for n in sorted(needs, key=lambda v: -v['depth']):
        k = n['kind']
        if NOISE.search(n['what']):
            continue
        if k == 'crate':
            # Whose side holds it and how far back the script chain tests it says nothing: the
            # player must walk over it (Weathertop's athelas: "PlyrCivilian has < 1", step 2).
            if goal and not n['bad'] and n['what'] not in seen:
                seen.add(n['what'])
                objects.append((n['what'], ''))
            continue
        if goal and k in ('destroy', 'kill', 'wipe'):  # "Aragorn Flag" in "hERO Death flags" is a target
            targets(m, names, n, places, later, objects, seen)
            continue
        if n['bad']:
            continue
        if goal and k == 'leave' and n.get('side') == 'enemy' and n['depth'] <= 3:
            pts = m.polys.get(n['what'])
            if n['what'] not in seen and pts and not big(m, pts):
                seen.add(n['what'])
                later.append(('очистить от врагов: ' + spoken(n['what']), n['x'], n['y'], n['via']))
            continue
        if goal and k == 'buildat' and n['depth'] <= 3:
            pts = m.polys.get(n['what'])
            if n['what'] not in seen and pts and not big(m, pts) and not WIDE.search(n['what']):
                seen.add(n['what'])
                later.append((f"построить {names.obj(n['type'])}: {spoken(n['what'])}", n['x'], n['y'], n['via']))
            continue
        if n.get('side') in ('enemy', 'bystander') or (n['depth'] > 1 and WIDE.search(n['what'])):
            continue
        if k in ('reach', 'camera'):
            pts = m.polys.get(n['what'])
            if n['what'] in seen or not pts or big(m, pts):
                continue
            seen.add(n['what'])
            who = n['who'] if n['who'] and n['who'] != m.me and not n['who'].startswith(('<', 'Plyr')) and '/' not in n['who'] else ''
            word = 'навести камеру: ' if k == 'camera' else ''
            places.append((word + spoken(n['what']) + (f' (для {who})' if who else ''), n['x'], n['y'], n['via']))
        elif k == 'capture' and n['x']:
            if n['what'] not in seen:
                seen.add(n['what'])
                places.append(('захватить: ' + spoken(n['what']), n['x'], n['y'], n['via']))
        elif k == 'pickup':
            t, x, y = spot(m, n['what'])
            if t and t not in seen:
                seen.add(t)
                objects.append((t, ''))
        elif k == 'near':
            # A talk or a meeting: the other unit, wherever it walks.
            units = [u for u in n['what'].split(' + ')]
            for u in units:
                t, x, y = spot(m, u)
                if t and m.side_of(u) != 'enemy' and u not in ('Frodo',) and t not in seen and n['dist'] <= 400:
                    seen.add(t)
                    objects.append((t, ''))
    return places + later, objects


def display(m, template):
    local = m.local_pickups.get(template.upper())
    name = m.strs.get(local[1].upper(), '') if local and local[1] else ''
    return ('предмет: ' + re.split(r'\s{2,}', name)[0].strip()) if name else ''


def main():
    aotr, out_path = sys.argv[1], sys.argv[2]
    lotr = read_str(os.path.join(aotr, 'data', 'lotr.str'))
    out = open(out_path, 'w', encoding='utf-8')
    out.write('; Generated by tools\\make_guides.py from the maps\' scripts (tools\\triggers.py). Do not edit\n'
              '; by hand: put corrections in guides.ini (a guide there replaces the one here).\n')
    total = {'objectives': 0, 'places': 0, 'objects': 0, 'paths': 0, 'say': 0, 'pre': 0}
    names = Names(aotr, lotr)
    for folder in T.campaign_maps(aotr):
        mdir = os.path.join(aotr, 'maps', folder)
        try:
            m = T.Mission(aotr, folder, lotr)
        except Exception as e:  # noqa: BLE001
            out.write(f'\n; {folder}: unreadable ({e})\n')
            continue
        names.for_map(mdir, m.strs)
        out.write(f'\n; ===== {folder}\n')
        for i, tag in enumerate(m.tags):
            d = m.objective(i)
            # Lord of the Rings: "Defend the gate": the gate falling ends it, it is no target.
            m.keep = bool(KEEP.search(m.text(tag)))
            chain = m.trace(m.done.get(i, []), m.barrier(i))
            phrases, extra = actions_for(m, chain, names, {k: (v[0], v[1], v[2], v[3]) for k, v in m.named.items()}, m.areas)
            places, objects = collect(m, d['goal'], names)
            # A pick-up defined in map.ini: the mod's object list does not know its name.
            objects = [(t, display(m, t)) for t, w in objects]
            take = [names.obj(t) if not w else w for t, w in objects if t.upper() in m.pickups][:5]
            if take:
                phrases.insert(0, 'подобрать, пройдя по предмету: ' + ', '.join(dict.fromkeys(take)))
            if not places and not objects and not phrases and not d['done']:
                continue
            total['objectives'] += 1
            out.write(f'[{tag}]\n; {m.text(tag)}\n')
            if phrases:
                total['say'] += 1
                out.write('say = Что сделать: ' + '; '.join(phrases[:5]) + '.\n')
            if d['done']:
                out.write(f"complete = {d['done'][0]}\n")
            best = None
            for words, x, y, via in places[:12]:
                total['places'] += 1
                out.write(f'place = {words} | {x:.0f} {y:.0f} | {via}\n')
                for lab, wx, wy in m.paths:
                    if (wx - x) ** 2 + (wy - y) ** 2 <= 60 ** 2:
                        q = (wx - x) ** 2 + (wy - y) ** 2
                        if best is None or q < best[0]:
                            best = (q, lab, x, y)
            for t, words in objects[:12]:
                total['objects'] += 1
                out.write(f'object = {t} | {words}\n')
            if best:
                total['paths'] += 1
                # The place it serves: an objective's chain can cross stories, and a path of
                # Rivendell once walked Bilbo out of Gollum's cave (log 2026-11-06 01:13:16).
                out.write(f'autopath = {best[1]} | {best[2]:.0f} {best[3]:.0f}\n')
            # What leads to the objective while it is not shown yet.
            lead, _ = collect(m, [n for n in d['lead'] if n['depth'] <= 6], names, goal=False)
            if lead:
                total['pre'] += 1
                out.write(f'[PRE:{tag}]\n')
                for words, x, y, via in lead[:6]:
                    out.write(f'place = {words} | {x:.0f} {y:.0f} | {via}\n')
    out.close()
    print(total)


if __name__ == '__main__':
    main()
