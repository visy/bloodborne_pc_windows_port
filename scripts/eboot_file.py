#!/usr/bin/env python3
"""Which file holds the game's executable: eboot.bin (a SELF) or eboot.elf (the raw ELF that some
dumpers, Itemzflow among them, write). eboot.bin wins when both exist. prepare.py reads either
form (parse_self accepts a plain ELF); this module is the one place that knows the names (from
Mrsuss60/bloodborne_pc_windows_port 2054129f / d7b2db11 by facundo1701, PR #189 there).
"""
import hashlib
from pathlib import Path

EBOOT_NAMES = ('eboot.bin', 'eboot.elf')


def find_eboot(game):
    """The executable file in the game folder (eboot.bin first), or None."""
    root = Path(game)
    for name in EBOOT_NAMES:
        path = root / name
        if path.is_file():
            return path
    return None


def eboot_path(game):
    """find_eboot, or the eboot.bin path (for a message about the file that is missing)."""
    return find_eboot(game) or Path(game) / EBOOT_NAMES[0]


def is_eboot_name(name):
    return str(name).casefold() in EBOOT_NAMES


def image_sha256(elf, ph):
    """SHA-256 of the loaded image (the load segments at their addresses): the same for the SELF
    and the raw ELF of one executable."""
    loads = [p for p in ph if p['type'] in (1, 0x61000010)]
    image = bytearray(max(p['vaddr'] + p['memsz'] for p in loads))
    for p in loads:
        image[p['vaddr']:p['vaddr'] + p['filesz']] = elf[p['offset']:p['offset'] + p['filesz']]
    return hashlib.sha256(image).hexdigest()


def reviewed_source_sha256(module, supported_image, supported_source_sha256):
    """The source hash the Legacy CPU plan was reviewed against is the clean SELF's. A raw ELF of
    the same executable (same loaded image as the clean 1.09) stands for it; any other file keeps
    its own hash."""
    if module.get('raw_elf') and \
            image_sha256(module['elf'], module['ph']) == supported_image:
        return supported_source_sha256
    return module.get('sha256')
