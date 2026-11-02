#!/usr/bin/env python3
"""Writes tests/name_filter_vectors.txt: the shared positive (+) and negative (-) names both filters must agree on.

  python tools/name_filter/gen_vectors.py --bb-texts bb_texts.json [--out tests/name_filter_vectors.txt]

Positives are built from a few dictionary words by evasion transforms (the file therefore holds the words it is meant to
catch; it is test data only). Negatives are the handwritten look-alikes below plus every Bloodborne name (items, weapons,
armour, NPCs, places, blood gems) taken from the game's English text dump; --bb-texts is a JSON {fmg: [strings]} made
from the user's own game files (tools/party/flag_tool.py readers), not part of the repo. Without it the negatives are only
the handwritten ones. No player names, no real logs: everything here is synthetic or game text.
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

BASE = ["nigger", "faggot", "hitler", "chink", "nazi", "retard", "kike", "wetback", "raghead", "pidor", "tranny", "spic", "cunt"]
FW = lambda s: "".join(chr(ord(c) + 0xFEE0) if "a" <= c <= "z" else c for c in s)
MB = lambda s: "".join(chr(0x1D41A + ord(c) - 97) if "a" <= c <= "z" else c for c in s)
MS = lambda s: "".join(chr(0x1D5BA + ord(c) - 97) if "a" <= c <= "z" else c for c in s)
MM = lambda s: "".join(chr(0x1D68A + ord(c) - 97) if "a" <= c <= "z" else c for c in s)
CI = lambda s: "".join(chr(0x24D0 + ord(c) - 97) if "a" <= c <= "z" else c for c in s)
PA = lambda s: "".join(chr(0x249C + ord(c) - 97) if "a" <= c <= "z" else c for c in s)
RI = lambda s: "".join(chr(0x1F1E6 + ord(c) - 97) if "a" <= c <= "z" else c for c in s)
SQ = lambda s: "".join(chr(0x1F130 + ord(c) - 97) if "a" <= c <= "z" else c for c in s)
CYR = {"a": "а", "e": "е", "o": "о", "p": "р", "c": "с", "x": "х", "i": "і", "y": "у"}
GRK = {"o": "ο", "a": "α", "i": "ι", "v": "ν", "k": "κ", "t": "τ", "e": "ε"}
SMALLCAP = dict(zip("abcdefghijklmnoprstuvwyz", "ᴀʙᴄᴅᴇꜰɢʜɪᴊᴋʟᴍɴᴏᴘʀꜱᴛᴜᴠᴡʏᴢ"))
ARM = {"o": "օ", "g": "ց", "n": "ո", "u": "ս"}
CHER = {"a": "Ꭺ", "e": "Ꭼ", "i": "Ꮖ", "k": "Ꮶ", "r": "Ꭱ", "t": "Ꭲ", "g": "Ꮐ"}
LEET = {"a": "4", "e": "3", "i": "1", "o": "0", "s": "5", "t": "7"}
UP = dict(zip("aefghikmnrtvwyudpbq", "ɐǝɟƃɥᴉʞɯ"
                                    "uɹʇʌʍʎ" "npdqb"))


def sub(m):
    return lambda s: "".join(m.get(c, c) for c in s)


def upside(s):
    return "".join(UP.get(c, c) for c in reversed(s))


def ins(sep):
    return lambda s: sep.join(s)


def comb(s):
    return "".join(c + "̸́" for c in s)


def zalgo(s):
    return "".join(c + "̖̗̀́̂̃҉" for c in s)


TRANSFORMS = [lambda s: s, str.upper, str.title, lambda s: s.upper() if len(s) % 2 else s.title(), FW, MB, MS, MM, CI, PA, RI, SQ,
              sub(CYR), sub(GRK), sub(SMALLCAP), sub(ARM), sub(LEET), upside,
              ins("."), ins("_"), ins(" "), ins("-"), ins("​"), ins("‍"), ins("⁠"), ins("﻿"), ins("­"),
              ins("͏"), ins("ㅤ"), ins("️"), ins("\U000e0041"), comb, zalgo,
              lambda s: "‮" + s[::-1], lambda s: "‮" + s, lambda s: "⁦" + s + "⁩",
              lambda s: "xX_" + s + "_Xx", lambda s: "~" + s + "~", lambda s: "-=" + s + "=-", lambda s: s + "99",
              lambda s: "".join(c * 3 if c in "aeiougs" else c for c in s), lambda s: s[0] + "*" * 0 + s[1:] + "!!"]
# leet symbols for specific words (hand written, the generic table above is digit leet only)
EXTRA_POS = [
    "n!gg3r", "n1gg3r", "n1gg4", "f4gg0t", "f@gg0t", "h1tl3r", "h!tler", "n4z1", "r3t4rd", "k1k3", "sp1c", "$pic",
    "W3tb4ck", "ниггер",
    "Пидор", "пидарас", "ХОХОЛ", "Жид", "Нацист", "гитлер", "Чурка",
    "p1d0r", "pidoras", "ch3rnozhopy", "zhid", "hohol", "gitler", "Neekeri", "Kanake", "Zenci", "Murzyn", "Frocio", "Maricon", "Sudaca",
    "Negrata", "Bougnoule", "Judensau", "Untermensch", "Kameltreiber", "Schwuchtel", "Culattone", "Kankerjood", "Czarnuch", "Ciapaty",
    "Heil Hitler", "Sieg Heil", "H3il H1tl3r", "S1eg H31l", "heilhitler", "1488", "14 88", "14/88", "xX1488Xx", "88 Heil", "14 words", "14words",
    "HH88", "88 ss", "White Power", "WhitePower", "white p0wer", "Whitepride", "KKK", "K K K", "k.k.k", "KKKK", "Ku Klux Klan klansman",
    "ww2 was right", "WW2WasRight", "Hitler was right", "Gas the Jews", "gasthejews", "Race War", "RaHoWa", "Death to Jews",
    "卐", "卐卐", "Hunter卐", "ᛋᛋ", "Third Reich", "Waffen SS", "Sandnigger", "Camel Jockey", "Neonazi",
    # phrase tier
    "Black ppl hunter", "B1ack p30ple hunt3r", "blackpplhunter", "BlackPeopleHunter", "Black people killer", "Blacks Hunter",
    "Black ppl slayer", "Kill all blacks", "Killallblacks", "Asians Hunter", "Jew Hunter", "Jew Killer", "Jew Burner", "Jews Die",
    "Jew Oven", "Jewish Exterminator", "Muslim Slayer", "Muslim Hunter", "Muslims Die", "Death to Muslims", "Gay Hater", "Gays Die",
    "Gay people hater", "Gay ppl hunter", "Trans ppl hunter", "Trans hater", "Transsexual hunter", "LGBT Hunter", "Lesbians Die",
    "Mexicans Hunter", "Mexican ppl hunter", "Chinese ppl killer", "Arabs Hunter", "Arab ppl slayer", "Hindus Die",
    "Hate Blacks", "Hating Jews", "Jew Gasser", "White ppl hater", "Whites Die", "Asian people killer", "Indians Hunter", "Japanese people hunter",
    "Africans Die", "Black ppl exterminator", "Black folks lyncher", "Lynch Blacks", "Lynch Jews", "Hang Gays", "Hang the Jews", "Noose for blacks",
    "Burn Muslims", "Bomb Muslims", "Muslim Bomber", "Mexican Hater", "Jew Hater", "Jew Haters", "xXBlackPplHunterXx", "BLACK PPL HUNTER 69", "B.l.a.c.k P.p.l H.u.n.t.e.r",
    "Black_ppl_hunter", "black-ppl-hunter", "Black​ppl​hunter", "Ｂｌａｃｋ ｐｐｌ ｈｕｎｔｅｒ",
    "вlack ppl hunter", "Blаck peоpӏe hunter".replace("ӏ", "l"), "\U0001d401\U0001d425\U0001d41a\U0001d41c\U0001d424 ppl hunter",
    "ⒷⓁⒶⒸⓀ ppl hunter", "Black ppl hunt3r", "Black ppl h u n t e r", "gypsies hunter", "i h8 blacks", "I h8 jews", "black p p l hunter", "Jews Gas Chamber",
]
NEGATIVES = """Black Hunter|Beast Hunter|Hunter of Hunters|Gehrman|Vileblood|Scunthorpe|Classic Assassin|Gay Gascoigne|Niger|Hunter|Black Cat|Kill Bill
White Hunter|The Old Hunter|Blood Hunter|Church Hunter|Cainhurst Knight|Choir Member|Executioner|Kin Hunter|Beast Slayer|Slayer of Beasts
Dragon Slayer|Vampire Hunter|Bounty Hunter|Treasure Hunter|Jewel Hunter|Jewel Thief|Jewelry|Jewett|Jewell|Hebrew Scholar|Muscle Man|Muslin Cloth
Gascoigne|Father Gascoigne|Lady Maria|Eileen the Crow|Djura|Alfred|Iosefka|Micolash|Ludwig|Laurence|Vicar Amelia|Rom the Vacuous|Ebrietas
Penistone|Dickens|Cocktail|Therapist|Analyst|Grape|Assassin|Classic|Class Act|Glass|Grass|Mass Effect|Bass Player|Brass Knuckle|Cassandra
Japan|Japanese Maple|Spice Girl|Spicy|Spicer|Spiced|Snigger|Sniggers|Niggle|Niggard|Cuntz|Dickson|Hancock|Shitake|Cockney|Peacock|Moscow
Essex|Sussex|Middlesex|Wessex|Arsenal|Titmarsh|Fanny Pack|Fanny Adams|Bastard Sword|Whitehouse|Blackwood|Blackburn|Blackpool|Whitby|Whitechapel
Brownie|Brown Bear|Black Sabbath|Black Panther|Black Widow|Black Mage|Black Knight|Black Hole|Black Friday|Black Tea|Black Cats Die Hard
White Walker|White Rabbit|White Knight|White Wolf|White Noise|Gay Paree|Gaylord|Gayle King|Gaynor|Trans Am|Transformer|Translator|Transit
Asian Tiger|Mexican Wave|Chinese Food|Indian Summer|Russian Doll|Polish Remover|French Fry|German Shepherd|Turkish Delight|Arab Horse|Roman Holiday
Death Knight|Death Stranding|Death Metal|Dead Hunter|Die Hard|Kill Switch|Kill Joy|Killjoy|Hate Crimes Unit|Haters Gonna Hate|Gas Station|Gas Mask|Burner Phone
Oven Mitt|Genocide Memorial|Lynch Pin|Lynchburg|David Lynch|Hunter S Thompson|Hunted|Hunters Moon|Moon Presence|Paleblood Hunter|Beast Cleanser
Mr Pink|Sir Hunter|Lord Hunter|xXHunterXx|HunterX|Hunter69|Hunter 3F2|Hunter 007|Yharnam Local|Old Yharnam|Cleric Beast|Father Gascoigne
Moon Presence|Mensis Scholar|Orphan of Kos|Amygdala|Shadows of Yharnam|Darkbeast Paarl|Vicar Amelia|Witch of Hemwick|Living Failures|Logarius
Yurie the Last Scholar|Brador|Adella|Arianna|Valtr|Henryk|Simon the Harrowed|Annalise|Gilbert|Hunters Mark|Ashen Blood|Fire Paper|Pebble
Gamer|Pro Gamer|xX_Sniper_Xx|Sniper Elite|NoScope|Ninja|Naruto|Sasuke|Kakashi|Goku|Vegeta|Link|Zelda|Mario|Luigi|Samus|Kratos|Atreus
Anna|Ivan|Olga|Sergey|Dmitry|Natasha|Pavel|Alexei|Nikolai|Vladimir|Kirill|Sasha|Masha|Dasha|Tanya|Katya|Misha|Igor|Oleg
Klaus|Hans|Greta|Jurgen|Lukas|Matthias|Sven|Ingrid|Bjorn|Olaf|Erik|Magnus|Freya|Thor|Odin|Loki|Hel|Valkyrie|Heimdall
Juan|Carlos|Maria|Jose|Luis|Pedro|Diego|Sofia|Lucia|Pablo|Miguel|Javier|Mateus|Joao|Ana|Bruno|Rafael|Thiago|Gabriel
Giovanni|Luca|Marco|Paolo|Francesca|Chiara|Matteo|Alessandro|Lorenzo|Valentina|Pierre|Jean|Luc|Marie|Camille|Antoine|Hugo|Louis
Mikko|Juha|Matti|Heikki|Ville|Jari|Tuomas|Aino|Eino|Kalle|Kimmo|Jussi|Pekka|Timo|Veli|Antti|Tero|Sami|Janne|Jukka
Ahmed|Mohammed|Fatima|Ali|Omar|Hassan|Yusuf|Layla|Nazir|Nazim|Nazar|Zahra|Karim|Samir|Rania|Hanan|Jamal|Tariq|Bilal|Idris
Wei|Li Na|Zhang|Wang|Chen|Liu|Kim|Park|Lee|Tanaka|Suzuki|Sato|Yamamoto|Nakamura|Kobayashi|Takahashi|Watanabe|Ito|Saito|Hayashi
Ozan|Emre|Burak|Mehmet|Elif|Zeynep|Selin|Cem|Deniz|Kerem|Tolga|Ceren|Eda|Ece|Hakan|Baris|Murat|Ayse|Fatma|Gokhan
Tomasz|Piotr|Katarzyna|Agnieszka|Michal|Marcin|Krzysztof|Jakub|Pawel|Wojciech|Dawid|Kamil|Adrian|Maciej|Rafal|Lukasz|Zofia
Joost|Daan|Sanne|Lotte|Femke|Bram|Ruben|Thijs|Jeroen|Wouter|Niels|Maarten|Pieter|Stijn|Sjoerd|Lars|Anneke|Marieke
Анна|Иван|Охотник|Темный Охотник|Кровавый|Сергей|Дмитрий|Хороший|Собака|Котик|Дима|Волк|Медведь|Хоя|Харков|Питер|Сказка
狩猟者|ハンター|上田|太郎|ゆうき|山田太郎|黒い犬|ブラックハンター|獵人|王|贝贝|李明|张伟|卢光|平和|卝|卍|한국|헌터|김민수|احمد|صياد|Σωκράτης|αλέξανδρος|काला|אבי
Hunter \U0001f480|\U0001f5e1️ Hunter|★ Hunter ★|☠ Beast ☠|❤ Hunter ❤|ᴴᵁᴺᵀᴱᴿ|Ｈｕｎｔｅｒ|\U0001d407\U0001d42e\U0001d427\U0001d42d\U0001d41e\U0001d42b|H́únter|ⒽⓊⓃⓉⒺⓇ|hunter​|Hu‍nter|‮retnuH
""".replace("\n", "|").split("|")


def main():
    out = ROOT / "tests" / "name_filter_vectors.txt"
    bb = None
    a = sys.argv[1:]
    if "--out" in a:
        out = Path(a[a.index("--out") + 1])
    if "--bb-texts" in a:
        bb = json.load(open(a[a.index("--bb-texts") + 1], encoding="utf-8"))
    pos = []
    for w in BASE:
        for t in TRANSFORMS:
            pos.append(t(w))
    pos += EXTRA_POS
    neg = [n for n in NEGATIVES if n.strip()]
    if bb:
        names = set()
        for k, v in bb.items():
            if any(x in k for x in ("名.fmg", "魔石名")):
                for s in v:
                    s = re.sub(r"\s+\(\d+\)$", "", s).strip()
                    if s and s != "*" and not s.startswith("en") and len(s) <= 40 and "\n" not in s and "NOT NEEDED" not in s:
                        names.add(s)
        neg += sorted(names)
    seen, lines = set(), ["# Shared name-filter test vectors: '+<TAB>name' must be masked, '-<TAB>name' must pass.",
                          "# Generated by tools/name_filter/gen_vectors.py (synthetic names and Bloodborne item / NPC / place names)."]
    for sign, items in (("+", pos), ("-", neg)):
        for n in items:
            n = n.replace("\t", " ").replace("\n", " ")
            if (sign, n) in seen or not n:
                continue
            seen.add((sign, n))
            lines.append(f"{sign}\t{n}")
    out.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    print(len([l for l in lines if l[0] == "+"]), "positive,", len([l for l in lines if l[0] == "-"]), "negative")


if __name__ == "__main__":
    main()
