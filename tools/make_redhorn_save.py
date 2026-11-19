"""Build a campaign save that continues after Council of Elrond into Redhorn (AotR 10.0 numbering).

Usage: python -I make_redhorn_save.py <Saved Game 18> <Auto Save for Campaign Council of Elrond> <out>

Why (2026-11-01): the player's manual saves descend from AotR 9.x saves (June 2025), where
Council of Elrond was mission index 4; AotR 10.0 inserted a mission before it, so Council is
index 5 and winning it from the old chain advances 4 -> 5 = Council again. The launcher's
"Campaign Save Update" fixes only "Auto Save for Campaign *" files: mission index + 1,
missions-completed + 1 for some heroes, a Debug_TEST carryover unit (campaign_manager.py
run_mass_upgrade). It rewrote the Council autosave that way at 08:38:52.

Input A = "Saved Game 18": saved in the campaign menu right after the Council victory
(index 5, no Debug_TEST). Input B = that autosave after the launcher's update (index 6,
Debug_TEST). A byte diff showed B = launcher update of A's state plus a different save header.
Output = B with A's header (GameState chunk, everything before CHUNK_Campaign) and A's
GameLogic mode byte, description "Redhorn Start": a between-missions save whose next
mission is index 6 = Redhorn. Nothing in either input is changed.
"""
import sys

ANG = b'ANGMAR_BONUS_CAMPAIGN'
LOGIC = b'CHUNK_GameLogicKOLB'
HEADER_END = 0x1B6          # end offset of CHUNK_GameState in both files (u32 after its KOLB)
DESC_OFF = 0x157            # UTF-16 description, 13 characters, length byte at 0x156
NEW_DESC = 'Redhorn Start'  # same length as "Saved Game 18" and "__AUTO#SAVE__"


def u32(d, o):
    return int.from_bytes(d[o:o + 4], 'little')


def main():
    a = open(sys.argv[1], 'rb').read()
    b = bytearray(open(sys.argv[2], 'rb').read())
    out = sys.argv[3]

    for name, d in (('A', a), ('B', b)):
        assert d[:8] == b'ALAE2STR', name + ': not a BfME2 save'
        assert u32(d, 0x13B) == HEADER_END, name + ': GameState chunk does not end at 0x1B6'
        assert d[0x156] == 13, name + ': description is not 13 characters'
    ia, ib = a.find(ANG) + 34, b.find(ANG) + 34
    assert a[ia] == 5 and b[ib] == 6, 'mission index: A must be 5 (Council), B 6 (Redhorn)'
    assert b'Debug_TEST' not in a and b'Debug_TEST' in b, 'Debug_TEST: A without, B with'

    b[:HEADER_END] = a[:HEADER_END]
    b[DESC_OFF:DESC_OFF + 26] = NEW_DESC.encode('utf-16-le')

    la, lb = a.find(LOGIC) + len(LOGIC) + 4, b.find(LOGIC) + len(LOGIC) + 4
    assert a[la:la + 13] == b[lb:lb + 13], 'GameLogic chunk starts differ'
    mode_a, mode_b = la + 13, lb + 13  # byte that differed: 06 in A (campaign menu), 02 in B
    assert a[mode_a] == 6 and b[mode_b] == 2, 'GameLogic mode byte not where expected'
    b[mode_b] = a[mode_a]

    assert b[ib] == 6 and u32(b, 0x13B) == HEADER_END and b.endswith(b'SG_EOF')
    open(out, 'wb').write(b)
    print(f'written {out}: {len(b)} bytes, mission index {b[ib]}, Debug_TEST present, description {NEW_DESC!r}')


if __name__ == '__main__':
    main()
