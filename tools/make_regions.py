"""Write data\\regions.txt: the regions of every War of the Ring region campaign.

Usage: python -I make_regions.py <aotr folder> <out.txt>

Source: data\\ini\\campaigns\\riskcampaign.ini, block "LivingWorldRegionCampaign <name>" and the
files it #includes (Region blocks). UTF-8, tab separated:
  campaign <name>
  region <id> <display name> <centre x> <centre y> <map> <neighbours, comma> <hero x y> <garrison x y> <plots: x y;x y>
"""
import glob
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from missions_audit import read_str  # noqa: E402


def uncomment(text):
    return '\n'.join(re.split(r';|//', ln, 1)[0] for ln in text.splitlines())


def xy(body, key):
    m = re.search(key + r'\s*=\s*X:\s*(-?[\d.]+)\s+Y:\s*(-?[\d.]+)', body)
    return (float(m.group(1)), float(m.group(2))) if m else None


def regions_of(text, strs):
    out = []
    for m in re.finditer(r'^\s*Region\s+(\S+)\s*$(.*?)(?=^\s*Region\s+\S+\s*$|\Z)', text, re.M | re.S):
        rid, body = m.group(1), m.group(2)
        key = re.search(r'DisplayName\s*=\s*(\S+)', body)
        name = strs.get(key.group(1).upper(), rid) if key else rid
        mp = re.search(r'MapName\s*=\s*"?([^"\n]*)"?', body)
        centre = xy(body, 'CenterPoint') or xy(body, 'HeroArmySpot') or xy(body, 'GarrisonArmySpot')
        if not centre:
            continue
        near = re.findall(r'Connection\s+Region\s*=\s*(\S+)', body)
        hero, gar = xy(body, 'HeroArmySpot') or centre, xy(body, 'GarrisonArmySpot') or centre
        plots = [(float(a), float(b)) for a, b in re.findall(r'BuildingSpot\s*=\s*X:\s*(-?[\d.]+)\s+Y:\s*(-?[\d.]+)', body)]
        out.append((rid, re.sub(r'\s+', ' ', name), centre, (mp.group(1).strip() if mp else ''), near, hero, gar, plots))
    return out


def main():
    aotr, out_path = sys.argv[1], sys.argv[2]
    ini = os.path.join(aotr, 'data', 'ini', 'campaigns')
    strs = read_str(os.path.join(aotr, 'data', 'lotr.str'))
    files = {os.path.relpath(p, ini).lower().replace('/', '\\'): p for p in glob.glob(os.path.join(ini, '**', '*.inc'), recursive=True)}
    text = uncomment(open(os.path.join(ini, 'riskcampaign.ini'), 'rb').read().decode('latin-1'))
    out = open(out_path, 'w', encoding='utf-8')
    total = 0
    for m in re.finditer(r'^LivingWorldRegionCampaign\s+(\S+)(.*?)(?=^LivingWorldRegionCampaign\s|\Z)', text, re.M | re.S):
        name, body = m.group(1), m.group(2)
        out.write(f'campaign\t{name}\n')
        for inc in re.findall(r'#include\s+"([^"]+)"', body):
            p = files.get(inc.lower())
            if not p:
                continue
            for rid, disp, c, mp, near, hero, gar, plots in regions_of(uncomment(open(p, 'rb').read().decode('latin-1')), strs):
                total += 1
                out.write('\t'.join(['region', rid, disp, f'{c[0]:.0f}', f'{c[1]:.0f}', mp, ','.join(near),
                                     f'{hero[0]:.0f} {hero[1]:.0f}', f'{gar[0]:.0f} {gar[1]:.0f}',
                                     ';'.join(f'{a:.0f} {b:.0f}' for a, b in plots)]) + '\n')
    out.close()
    print(total, 'regions')


if __name__ == '__main__':
    main()
