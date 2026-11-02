#!/usr/bin/env python3
"""Generates data/name_filter/charmap.tsv, the character table both name filters (Python and C++) load.

  python tools/name_filter/gen_charmap.py PATH/TO/confusables.txt [--out data/name_filter/charmap.tsv]

confusables.txt is the Unicode TR39 data file (https://www.unicode.org/Public/security/latest/confusables.txt, see
data/name_filter/UNICODE_LICENSE.txt). It is read here only; the filters never touch Unicode data or `unicodedata` at
run time, so Python and C++ cannot disagree about a character.

Line format (tab separated), codepoints in hex, [A-F]:
  HEX[-HEX]  drop        invisible / combining / format / control: removed
  HEX[-HEX]  sep         punctuation, symbols, spaces: a word separator
  HEX        =TEXT       replaced by TEXT (UTF-8; lower case; usually ASCII a-z0-9)
  HEX        ^TEXT       the same, and the character counts as an upside-down letter (see namefilter spec)
Everything not listed stands for itself (letters of other scripts, CJK, ...).
Rules applied per codepoint >= 0x80: NFKC, casefold, NFD, drop combining marks; when the result is not ASCII the
TR39 skeleton target of the character (or of its lower case) is used when it is plain ASCII a-z0-9 and at most 2 long.
"""
import sys, unicodedata as ud, argparse

FORCE_DROP = [(0x115F, 0x1160), (0x3164, 0x3164), (0xFFA0, 0xFFA0), (0x034F, 0x034F), (0x180B, 0x180E),
              (0x2060, 0x2064), (0xFEFF, 0xFEFF), (0x00AD, 0x00AD), (0x200B, 0x200F), (0x202A, 0x202E),
              (0x2066, 0x2069), (0xFE00, 0xFE0F), (0xE0100, 0xE01EF), (0xE0000, 0xE007F), (0x061C, 0x061C),
              (0x1BCA0, 0x1BCA3), (0x17B4, 0x17B5)]
EXTRA = {  # Latin look-alikes TR39 / NFKC leave alone
    "æ": "ae", "Æ": "ae", "œ": "oe", "Œ": "oe", "ø": "o", "Ø": "o", "đ": "d", "Đ": "d", "ð": "d", "Ð": "d",
    "ħ": "h", "Ħ": "h", "ł": "l", "Ł": "l", "ŀ": "l", "ı": "i", "ŧ": "t", "Ŧ": "t", "ĸ": "k", "ŋ": "n", "Ŋ": "n",
    "þ": "th", "Þ": "th", "ƒ": "f", "ǝ": "e", "ɛ": "e", "ɜ": "e", "ʃ": "s", "ƶ": "z", "ɨ": "i", "ɩ": "i", "ʋ": "v",
    "ʀ": "r", "ɴ": "n", "ɢ": "g", "ʟ": "l", "ʜ": "h", "ɪ": "i", "ᴀ": "a", "ʙ": "b", "ᴄ": "c", "ᴅ": "d", "ᴇ": "e",
    "ꜰ": "f", "ᴊ": "j", "ᴋ": "k", "ᴍ": "m", "ᴏ": "o", "ᴘ": "p", "ꞯ": "q", "ꜱ": "s", "ᴛ": "t", "ᴜ": "u", "ᴠ": "v",
    "ᴡ": "w", "ʏ": "y", "ᴢ": "z", "ꓘ": "k", "ꓡ": "l", "ꓭ": "b", "ꓛ": "c", "ꓷ": "d", "ꓞ": "i",
    # Greek and Cyrillic shapes TR39 does not list or lists differently
    "α": "a", "β": "b", "ε": "e", "ι": "i", "κ": "k", "ν": "v", "ο": "o", "ρ": "p", "τ": "t", "υ": "u", "χ": "x",
    "ω": "w", "η": "n", "μ": "u", "π": "n", "в": "b", "н": "h", "к": "k", "м": "m", "т": "t", "р": "p", "с": "c",
    "у": "y", "х": "x", "а": "a", "е": "e", "о": "o", "і": "i", "ј": "j", "ѕ": "s", "ԁ": "d", "ɡ": "g", "ѵ": "v",
    "ӏ": "i", "ґ": "r", "ь": "b", "я": "r", "ч": "4", "з": "3", "б": "6", "ё": "e", "й": "u", "ї": "i", "ѡ": "w",
}
UPSIDE = {"ɐ": "a", "ɔ": "c", "ǝ": "e", "ɟ": "f", "ƃ": "g", "ɥ": "h", "ᴉ": "i", "ɾ": "r", "ɯ": "w", "ɹ": "r",
          "ʇ": "t", "ʌ": "v", "ʍ": "m", "ʞ": "k", "ʎ": "y", "ɓ": "b", "ɐ̀": "a", "ǝ̀": "e", "ɘ": "e", "ᴚ": "r",
          "ʁ": "r", "ȝ": "e", "ǝ": "e", "ʃ": "s", "ᵷ": "g", "ɒ": "a", "ᴎ": "n", "ꓤ": "r", "ꓶ": "l", "ꓵ": "d"}


def load_conf(path):
    conf = {}
    for line in open(path, encoding="utf-8-sig"):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        parts = [p.strip() for p in line.split(";")]
        if len(parts) < 2:
            continue
        try:
            src = int(parts[0], 16)
            tgt = "".join(chr(int(x, 16)) for x in parts[1].split())
        except ValueError:
            continue
        conf[src] = tgt
    return conf


def strip_marks(s):
    return "".join(ch for ch in ud.normalize("NFD", s) if ud.category(ch) not in ("Mn", "Me"))


def ascii_ok(t):
    return t and len(t) <= 2 and all("a" <= ch <= "z" or "0" <= ch <= "9" for ch in t)


def fix_target(t, c):
    """Skeleton target -> our letter. TR39 folds i, l, I and 1 to l, and O to 0."""
    t = ud.normalize("NFKC", t).casefold()
    if t == "l":
        nm = ud.name(c, "")
        if (" LETTER I" in nm or nm.endswith(" I") or "DOTLESS I" in nm or "IOTA" in nm or "SMALL I" in nm
                or nm.endswith(" IOTA") or "BYELORUSSIAN-UKRAINIAN I" in nm):
            return "i"
    if t == "0" and not c.isdigit():
        return "o"
    if t == "1" and not c.isdigit():
        return "l"
    return t


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("confusables")
    ap.add_argument("--out", default="data/name_filter/charmap.tsv")
    a = ap.parse_args()
    conf = load_conf(a.confusables)
    force = set()
    for lo, hi in FORCE_DROP:
        force.update(range(lo, hi + 1))
    kinds = {}  # cp -> ("drop",) | ("sep",) | ("=", text) | ("^", text)
    for cp in range(0x80, 0x110000):
        if 0xD800 <= cp <= 0xDFFF:
            continue
        c = chr(cp)
        cat = ud.category(c)
        if cp in force or cat in ("Cf", "Cc", "Mn", "Me", "Co"):
            kinds[cp] = ("drop",)
            continue
        if cat == "Cn":
            continue
        lc = c.casefold()
        if c in UPSIDE or lc in UPSIDE:
            kinds[cp] = ("^", UPSIDE.get(c) or UPSIDE[lc])
            continue
        if 0x1F1E6 <= cp <= 0x1F1FF:
            kinds[cp] = ("=", chr(ord("a") + cp - 0x1F1E6))
            continue
        if c in EXTRA or lc in EXTRA:
            kinds[cp] = ("=", EXTRA.get(c) or EXTRA[lc])
            continue
        s = strip_marks(ud.normalize("NFKC", c).casefold())
        s = ud.normalize("NFKC", s)
        if s and all(ord(ch) < 0x80 for ch in s):
            if s != c:
                if s.isspace():
                    kinds[cp] = ("sep",)
                else:
                    kinds[cp] = ("=", s)
            continue
        if cat[0] in "LN" or cat == "Nl":
            t = conf.get(cp)
            if t is None and len(s) == 1:
                t = conf.get(ord(s))
            if t is not None:
                t = fix_target(t, c)
                if ascii_ok(t):
                    kinds[cp] = ("=", t)
                    continue
            if s and s != c:
                kinds[cp] = ("=", s)
            continue
        if cat[0] in "PSZ":
            kinds[cp] = ("sep",)
    lines = ["# Generated by tools/name_filter/gen_charmap.py from Unicode data (NFKC/casefold/NFD) and TR39 confusables.txt",
             "# (see UNICODE_LICENSE.txt). Do not edit by hand. Format: see the generator's docstring."]
    cps = sorted(kinds)
    i = 0
    while i < len(cps):
        cp = cps[i]
        k = kinds[cp]
        if k[0] in ("drop", "sep"):
            j = i
            while j + 1 < len(cps) and cps[j + 1] == cps[j] + 1 and kinds[cps[j + 1]] == k:
                j += 1
            lines.append(f"{cp:X}" + (f"-{cps[j]:X}" if j > i else "") + "\t" + k[0])
            i = j + 1
        else:
            lines.append(f"{cp:X}\t{k[0]}{k[1]}")
            i += 1
    open(a.out, "w", encoding="utf-8", newline="\n").write("\n".join(lines) + "\n")
    print(len(lines) - 2, "lines")


if __name__ == "__main__":
    main()
