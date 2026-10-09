#!/usr/bin/env python3
# tools/menu_en.py: translates the Russian in-game overlay menu (Insert / L3+R3) of
# out/bbport.exe to English, in place.
#
# Strings are matched by their exact Russian text (tools/bbport_menu_en.json), not by
# offset, so this reruns on future builds: every NUL-terminated copy of a known string is
# overwritten with its English text (same or shorter UTF-8 length, NUL-padded, identical
# printf format specifiers). Russian strings the table does not know are listed at the end;
# add them to the JSON and rerun. Rerunning on a patched exe changes nothing.
#
# Usage: python tools/menu_en.py [path/to/bbport.exe] [--check]
#   --check  reports what would change without writing.
# Before patching, the Russian exe is saved as bbport.exe.ru.bak (replaced when a newer
# build is patched, kept when an already partly translated exe is). Close the game first.
import json, os, re, sys

FMT = re.compile(r'%[-+ #0]*\d*(?:\.\d+)?(?:ll|l|z|h)?[a-zA-Z%]')
# A NUL-delimited run of ASCII / 2-byte Cyrillic / common punctuation (— « » × ° …).
STRING = re.compile(rb'(?<=\x00)(?:[\x09\x0a\x20-\x7e]|[\xd0-\xd1][\x80-\xbf]|\xe2\x80[\x93\x94\xa6]|'
                    rb'\xc2[\xa0\xab\xb0\xb7\xbb]|\xc3\x97)+(?=\x00)')
CYRILLIC = re.compile('[Ѐ-ӿ]{3}')


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    check = '--check' in sys.argv
    here = os.path.dirname(os.path.abspath(__file__))
    exe = args[0] if args else os.path.join(here, '..', 'out', 'bbport.exe')
    with open(os.path.join(here, 'bbport_menu_en.json'), encoding='utf-8') as f:
        table = json.load(f)

    errors = []
    for ru, en in table.items():
        if len(en.encode('utf-8')) > len(ru.encode('utf-8')):
            errors.append(f'too long for {ru[:40]!r}: {en!r}')
        if FMT.findall(ru) != FMT.findall(en):
            errors.append(f'format mismatch for {ru[:40]!r}: {FMT.findall(ru)} vs {FMT.findall(en)}')
    if errors:
        sys.exit('bbport_menu_en.json:\n  ' + '\n  '.join(errors))

    data = bytearray(open(exe, 'rb').read())
    patched, unknown = 0, []
    for m in STRING.finditer(bytes(data)):
        text = m.group().decode('utf-8')
        if not CYRILLIC.search(text):
            continue
        en = table.get(text)
        if en is None:
            unknown.append((m.start(), text))
            continue
        new = en.encode('utf-8')
        data[m.start():m.end()] = new + b'\0' * (m.end() - m.start() - len(new))
        patched += 1

    print(f'{exe}: {patched} strings to translate, {len(unknown)} unknown Russian strings')
    for off, text in unknown:
        print(f'  unknown at {off}: {text!r}')
    if not patched or check:
        if not patched:
            print('nothing to do (already English, or no known strings in this build)')
        return

    # Back up only a fully Russian exe: a partly translated one (an earlier run with an older
    # table) must not replace the original saved then.
    backup = exe + '.ru.bak'
    original = open(exe, 'rb').read()
    partly = any(b'\0' + en.encode('utf-8') + b'\0' in original
                 for en in table.values() if len(en) >= 20)
    if not partly or not os.path.exists(backup):
        with open(backup, 'wb') as f:
            f.write(original)
        print(f'original saved as {backup}')
    with open(exe, 'wb') as f:
        f.write(data)
    print('patched')


main()
