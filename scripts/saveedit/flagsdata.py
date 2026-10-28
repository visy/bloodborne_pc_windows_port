"""The event flags the editor offers by name: boss kills, lit lamps, ending flags.

The ids are the lists scripts/party_session.py checks after a party session (tests/test_saveedit.py keeps
them equal). Only names with evidence in docs/party (campaign_soak.md) are given; the rest are labelled
by their map (flag id 1AAB18xx: boss kill of map mAA_B, 7AAB02xx: lamp of map mAA_B).
"""
BOSS_IDS = (12101800, 12101850, 12201800, 12301800, 12401800, 12411800, 12421800, 12501800, 12601800,
            12601850, 12701800, 12801800, 13201800, 13301800, 13401800, 13401850, 13501800, 13501850,
            13601800)
LAMP_IDS = (72110200, 72200200, 72200201, 72300200, 72300201, 72300202, 72400200, 72400201, 72410200,
            72410201, 72410202, 72410203, 72420200, 72420201, 72420202, 72500200, 72500201, 72500202,
            72600200, 72600201, 72600202, 72600203, 72700200, 72700201, 72800200, 72800201, 72800202,
            72800203)
ENDING_IDS = (21, 22, 23, 6600, 6601, 6602, 6603, 6604)
KNOWN_BOSS = {12101800: "Gehrman", 12401800: "Vicar Amelia", 13201800: "Rom, the Vacuous Spider"}


def _map(fid):
    aa, b = (fid // 10**5) % 100, (fid // 10**4) % 10
    return f"m{aa}_{b:02d}"


def bosses():
    return [(f, f"{KNOWN_BOSS.get(f, 'Boss kill')} ({f}, {_map(f)})") for f in BOSS_IDS]


def lamps():
    return [(f, f"Lamp {f} ({_map(f)})") for f in LAMP_IDS]


def endings():
    return [(f, f"Ending flag {f}") for f in ENDING_IDS]


def boss_revive_flags(fid):
    """Flags cleared to revive a boss: only its kill flag (the table has no other flag proven to belong to it)."""
    return [fid]
