"""The two-PC session plan: every session (one game start on each PC) with the test aids it sets.

Data only (runner.py runs it, verdict.py judges it). pc1 is the host, pc2 the guest (in 1e the invader).
Each session is a copy of a smoke_party.py phase (tools/mp/smoke_party.py) with the 0.8.1 defaults, so its
director chains are the ones the one-PC smoke already proves; the names in the comments are those phases.
No game data is written here: lamp ids, regions and entity ids are the same numbers the smoke and the docs
use, and they only go to the game as BB_PARTY_DIRECTOR_TEST text.

Syncing the PCs: never by wall clock. A session starts on pc2 when pc1 says its game is up (the party code
exists), or when pc1's log shows an event (`start`); it ends on each PC when its own `until` markers have been
seen (plus `hold` s), and a PC leaves the session when the other one is done too (or `grace` s have passed).
"""

TOMB, WARD, DREAM = "2412953", "2402950", "2102950"      # the lamps the smoke uses (m24_01, m24_00, m21_00)
CLERIC_CLEAR = "12411700,12411701,12411702,12411703,12414700,12414701,12414223,72410511,2411,9456,2410"
CLERIC_TRIGGER_HOST = "-136.5:-26.6:52.8"
CLERIC_SPOT_GUEST = "-134.6:-26.6:54.7"
LAST_STEP = r"travel step (\d+)/\1: "                      # a director chain's last step started

# bbport.ini of every session (the smoke's: 720p, upscaler off, no post effects: a PC of any speed loads alike)
INI = ("upscaler=off\npreset=0\noutput_res=1280x720\nlive_resolution=0\nshow_fps=1\n"
       "skip_intro=1\nmodel_lod=1\neffect_chromatic_aberration=0\neffect_dof=0\neffect_motion_blur=0\n"
       "effect_ssao=0\neffect_game_aa=0\neffect_dynamic_shadows=0\neffect_ssr=0\n")


def S(**kw):
    d = dict(
        minutes=5, seconds=480, hold_host=20, hold_guest=20, grace=150,
        save_host="savedata", save_guest="savedata",
        env=[], host_env=[], guest_env=[], host="god", guest="god",
        host_until=[], guest_until=[], start="code", signals={}, pad_host=False,
        net_sim=None, vpn=False, optin=False, ini_host=[], ini_guest=[])
    d.update(kw)
    return d


def sessions():
    out = []
    # 1a: seamless travel with the 0.8.1 defaults (no seam toggle is set). Smoke pf0 / mk: the party warps to a lamp, to the
    # Hunter's Dream, then the host uses the Hunter's Mark from the Dream (the party goes back to the last lamp).
    out.append(S(
        id="1a.travel", block="1", title="seamless party travel: lamp, Dream, Hunter's Mark", minutes=6,
        env=["BB_STALL_PROBE=100"],
        host_env=["BB_PARTY_SPLIT_TEST=yes"],
        host=",".join(["god", "host_warp=%s@10^1" % TOMB, "host_warp=%s@15%%m24_01" % WARD, "host_warp=%s@15%%m24_00" % DREAM,
                       "host_lastlamp=%s@5%%m21_00^1" % WARD, "host_use=100@2%m21_00"]),
        host_until=[r"travel step (\d+)/\1: host_use"], hold_host=50, hold_guest=50))
    # 1a: Rejoin in place (smoke qr1, the 0.8.1 default party_quick_rejoin): Party > Rejoin pressed in the host's world.
    out.append(S(
        id="1a.rejoin", block="1", title="Rejoin in place (one loading screen beside the host)", minutes=4, seconds=300,
        guest="god,guest_press=0@10!,guest_rejoin:now@8",
        host_until=[r"cooperators 0 -> 1"], guest_until=[r"guest: re-join done after"], hold_host=40, hold_guest=15))
    # 1a: a join while the host travels. Both PCs are in the party after the first warp; the guest presses Party > Leave 8 s after
    # it arrives and Party > Rejoin 6 s after it is out; the host's second warp starts 1 s after the guest's rejoin has risen the
    # host's cooperators again ("^1*3": the third rise (join, arrival after warp 1, rejoin)), so the host leaves for the Cathedral Ward while the guest is still
    # settling in. Each side's delays count from its own events (arrival, leave, the count), never from a shared clock.
    out.append(S(
        id="1a.join", block="1", title="a rejoin right before the host travels (the guest is still arriving)", minutes=5, seconds=420,
        host="god,host_warp=%s@10^1,host_warp=%s@1^1*3" % (TOMB, WARD),
        guest="god,guest_leave@8%m24_01,guest_rejoin@6",
        host_until=[LAST_STEP], guest_until=[r"map [^>]*-> m24_00"], hold_host=40, hold_guest=20, grace=200))
    # 1c: C2, the host dies in a boss fight, the boss goes on at the living guest (smoke hdr: the Cleric Beast, default save).
    out.append(S(
        id="1c", block="1", title="C2: the host dies in a boss fight, the boss keeps attacking the guest", minutes=7, seconds=600,
        env=["BB_PARTY_PROGRESS_TEST_CLEAR=" + CLERIC_CLEAR, "BB_PARTY_TARGET_ANIM=1", "BB_PARTY_AI_LOG=1", "BB_BOSS_LOG=1"],
        host_env=["BB_PARTY_ROT_TEST_FRAIL=1"], guest_env=["BB_PARTY_TRAVEL_CHOICE=alone"],
        host=",".join(["goods=4114:0", "host_warp=2412952@8^1", "host_move=%s@45%%m24_01" % CLERIC_TRIGGER_HOST,
                       "host_chr=2410800:2@10%m24_01", "host_die@3%m24_01"]),
        guest=",".join(["god_bar=20", "guest_die@20%m24_01", "guest_press=0x0@25%m24_01",
                        "guest_move=%s@1%%m24_01" % CLERIC_SPOT_GUEST, "guest_chr=2410800:8@3%m24_01?host=spec",
                        "guest_press=0x0@90%m24_01", "guest_press=0x0@2%m24_01"]),
        host_until=[LAST_STEP], guest_until=[LAST_STEP], hold_host=10, hold_guest=6, grace=200))
    # 1d: a Chalice dungeon across game regions (smoke crg): pc2 runs the "Unlock Game Region" patch, pc1 does not.
    out.append(S(
        id="1d", block="1", title="Chalice across regions (pc2 has the region patch)", minutes=5, seconds=420, optin=True,
        save_guest="chalice_pthumeru_ritual", pad_host=True,
        env=["BB_PARTY_TRAVEL_CHOICE=party", "BB_PARTY_ALTAR_AUTO=1"],
        guest_env=["BB_PATCHES=Skip Intro;Unlock Game Region"],
        host=("log_pos,place=0x15000000:-15.5:-5.43:-16.9:0,approach=-17.1:-16.9:0.2,face=-18.27:-16.88,prompt=First Ritual Altar,"
              "seq=cross/?cross@Use the Ritual Altar/wait/down/down/cross,host_press=0@40%m29!"),
        guest="log_pos", host_until=[r"map \S* ?-> m29_00"], guest_until=[r"map \S* ?-> m29_00"], hold_host=20, hold_guest=20))
    # 1e: the invader. pc2 dials as soon as pc1's game has a party code (the host is still booting: no world yet) and keeps asking
    # quietly; the host travels while the invader is inside (the invasion ends, 0.8.1: no modal box, a retry).
    out.append(S(
        id="1e", block="1", title="invader dials before the host is in a world; the host travels with it inside", minutes=8, seconds=660,
        env=[], host_env=["BB_PARTY_INVADERS=1", "BB_PARTY_INVADE_APART_ANY=1", "BB_PARTY_DIRECTOR_TEST_SOLO=1"],
        guest_env=["BB_PARTY_INVADE=1", "BB_PARTY_INVADE_TEST=rejoin@25?home,rejoin@25?home"],
        host="god,place=0x18010000:-193.5:-28.6:68.6:0,host_warp=%s@100" % WARD, guest="",
        host_until=[LAST_STEP], guest_until=[r"invade: notice: You invade "], hold_host=40, hold_guest=60, grace=240))
    # 2: a repeat of 1a (travel) and 1c under BB_NET_SIM=dsl on the guest.
    a = dict(out[0], id="2a", block="2", title="seamless party travel, guest on simulated DSL", net_sim="dsl")
    c = dict(out[3], id="2c", block="2", title="C2 boss death, guest on simulated DSL", net_sim="dsl", seconds=660)
    out += [a, c]
    # 3: both PCs on the Nightwatch VPN (NW-03d11e5). The guest joins with the code the host's game published (party_code.txt, the
    # LAN/VPN slot: a full tunnel address on the VPN), handed over by the beacon; one party warp. 3b: the same through the VPN lobby row
    # (fallback only). Both hosts also announce to the lobby.
    out.append(S(
        id="3", block="3", title="Nightwatch VPN: join by the host's published party code, one party warp", minutes=7, seconds=600, vpn=True,
        host="god,host_warp=%s@10^1" % TOMB, ini_host=["party_vpn_lobby=1"],
        host_until=[LAST_STEP], guest_until=[r"map \S* ?-> m24_01"], hold_host=40, hold_guest=30))
    out.append(dict(out[-1], id="3b", title="Nightwatch VPN fallback: join from the lobby row", start="vpn"))
    return out


def select(all_sessions, only=None, skip=None):
    """Sessions chosen by --only / --skip: a token is a session id or the part before the dot ('1a' = 1a.*, '1' = block 1)."""
    def hit(s, tok):
        return s["id"] == tok or s["id"].split(".")[0] == tok or s["block"] == tok
    chosen = [s for s in all_sessions if not only or any(hit(s, t) for t in only)]
    return [s for s in chosen if not skip or not any(hit(s, t) for t in skip)]
