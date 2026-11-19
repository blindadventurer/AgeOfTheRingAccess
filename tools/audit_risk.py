"""Which campaign objectives a blind player is likely to get stuck in: the ones the generated
guides (make_guides.py) cannot walk well, so they need a hand-written chain of steps in
data\\guides.ini. Read-only: it reads the maps and writes a report.

Usage: python -I audit_risk.py <aotr folder> <guides.ini> <report.txt> [map filter]

Signals per objective (triggers.Mission model: the chain of scripts that completes it, the
lead-in that shows it):
  open   a shut way (HUMAN_IMPASSABLE_AREA area 0) opens inside the objective's own chain:
         the place is behind it until a middle stage is done (Barrows' small gate).
  loose  (per map, not scored) shut ways of a story region whose opener no chain holds: the
         way opens by a walk or a pick-up the chains do not follow, so the generated guide never
         names it (Trollshaws 2026-11-19: the camp behind "Stone Trolls", opened at the farmstead).
  stages several different acts (walks, pick-ups, hiding, nearness, selections) one after
         another before the end.
  spawn  pick-ups teleported in by the chain: they are not there when the objective shows.
  gate   GATE_IS_OPEN: a lever or a gate the player toggles.
  hide   a hiding or stealth status is tested.
Score = 3 open + stages + 2 spawn + 2 gate + 2 hide. Objectives with a hand guide
are listed with "уже вручную" and score 0.
"""
import os
import re
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import triggers  # noqa: E402
from missions_audit import read_str  # noqa: E402

GENERIC = re.compile(r'^(objectives?|audio|music|debug|win|lose|loss|setup|init|cinematics?|global|misc|timers?|'
                     r'flags?|counters?|ai|fail|spawn|spawns|reinforcements?)\b', re.I)
ACTS = ('reach', 'crate', 'pickup', 'near', 'status', 'power', 'select', 'capture', 'leave')


def hand_tags(path):
    tags = set()
    for ln in open(path, encoding='utf-8-sig'):
        ln = ln.strip()
        if ln.startswith('[') and ln.endswith(']'):
            t = ln[1:-1].upper()
            if not t.startswith(('PRE:', 'HINT:')):
                tags.add(t)
    return tags


def region_of(s):
    for part in s['group'].split('/'):
        p = part.strip()
        if p and not GENERIC.match(p):
            return p.split(' - ')[0].strip().lower()
    return ''


def audit(m, hand):
    openers = []  # (script, area)
    for s in m.scripts:
        for a, st, args in s['actions']:
            if a == 'HUMAN_IMPASSABLE_AREA' and len(args) >= 2 and args[0][3] and not args[1][1] and not args[1][2]:
                openers.append((s, args[0][3]))
    chains = {}
    for i in range(len(m.tags)):
        stop = m.barrier(i)
        done = m.trace(m.done.get(i, []), stop)
        lead = m.trace(m.show.get(i, []), stop)
        chains[i] = (done, lead)
    owned = set()
    for i, (done, lead) in chains.items():
        for s, d in done + lead:
            owned.add(id(s))
    rows = []
    for i, tag in enumerate(m.tags):
        done, lead = chains[i]
        # The objective's own region: that of the nearest script with one. The chain walks on into
        # other regions (Grey Company's travel circles in Annuminas lead to every region): those
        # scripts are left out.
        main = next((region_of(s) for s, d in sorted(done, key=lambda v: v[1]) if region_of(s)), '')
        if main:
            done = [(s, d) for s, d in done if region_of(s) in ('', main)]
        ids = {id(s): d for s, d in done}

        sig = Counter()
        notes = []
        for s, area in openers:
            if id(s) in ids and ids[id(s)] >= 1:
                sig['open'] += 1
                notes.append(f'open {area!r} by {s["name"]!r}')
        needs = [n for n in m.needs(done) if not n['bad'] and n['depth'] >= 1 and n['kind'] in ACTS and n['side'] != 'enemy']
        acts = {(n['kind'], n['what']) for n in needs}
        if len(acts) >= 2:
            sig['stages'] += len(acts)
            notes.append('stages ' + '; '.join(f'{k} {w}' for k, w in sorted(acts))[:300])
        for s, d in done:
            for a, st, args in s['actions']:
                if a == 'UNIT_TELEPORT_TO_WAYPOINT' and args:
                    t = m.named.get(args[0][3], ('',))[0]
                    if t and t.upper() in m.pickups:
                        sig['spawn'] += 1
                        notes.append(f'spawn {args[0][3]!r} ({t})')
            for c, st, args in s['conditions']:
                if c == 'GATE_IS_OPEN':
                    sig['gate'] += 1
                    notes.append(f'gate {args[0][3] if args else ""!r}')
                elif c == 'UNIT_HAS_OBJECT_STATUS' and triggers.arg(args, triggers.A_STATUS) in ('HIDDEN', 'STEALTHED'):
                    sig['hide'] += 1
        score = 3 * sig['open'] + sig['stages'] + 2 * sig['spawn'] + 2 * sig['gate'] + 2 * sig['hide']
        handed = tag.upper() in hand
        rows.append((0 if handed else score, i, tag, m.text(tag), dict(sig), notes, handed))
    # Shut ways no objective's chain opens: opened by a walk or a pick-up the chains do not follow,
    # so no generated guide names the place that opens them (listed per region, not scored).
    loose = {}
    for s, area in openers:
        if id(s) not in owned and not triggers.BAD.search(s['name'] + ' ' + s['group']):
            loose.setdefault(region_of(s) or '-', set()).add(f'{area!r} by {s["name"]!r}')
    return rows, loose


def main():
    aotr, guides, out_path = sys.argv[1], sys.argv[2], sys.argv[3]
    flt = sys.argv[4].lower() if len(sys.argv) > 4 else ''
    hand = hand_tags(guides)
    lotr = read_str(os.path.join(aotr, 'data', 'lotr.str'))
    out = open(out_path, 'w', encoding='utf-8')
    total = Counter()
    summary = []
    for folder in triggers.campaign_maps(aotr):
        if flt and flt not in folder.lower():
            continue
        try:
            m = triggers.Mission(aotr, folder, lotr)
        except Exception as e:  # noqa: BLE001
            out.write(f'\n=== {folder}: unreadable ({e})\n')
            continue
        rows, loose = audit(m, hand)
        risky = [r for r in rows if r[0] >= 3]
        total['objectives'] += len(rows)
        total['risky'] += len(risky)
        total['hand'] += sum(1 for r in rows if r[6])
        summary.append((folder, len(rows), len(risky), sum(r[0] for r in rows)))
        out.write(f'\n=== {folder}: {len(rows)} objectives, {len(risky)} risky\n')
        for score, i, tag, text, sig, notes, handed in rows:
            mark = 'уже вручную' if handed else ('RISK %d' % score if score >= 3 else 'ok %d' % score)
            out.write(f'  [{i}] {mark:12s} {tag}: {text[:110]}\n')
            if score >= 3 or handed:
                out.write(f'        {sig}\n')
                for n in notes[:12]:
                    out.write(f'        - {n}\n')
        if loose:
            out.write('  shut ways no chain opens, by region:\n')
            for reg, lst in sorted(loose.items()):
                out.write(f'    {reg}: ' + '; '.join(sorted(lst)) + '\n')
            total['loose'] += sum(len(v) for v in loose.values())
    out.write('\n=== summary (map, objectives, risky, score)\n')
    for row in sorted(summary, key=lambda r: -r[3]):
        out.write('  %-45s %3d %3d %4d\n' % row)
    out.close()
    print(dict(total))


if __name__ == '__main__':
    main()
