from paths import ROOT
import struct
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

import patches

from patches import (EBOOT_BASE, OUTPUT_SIZE, RESOLUTION_TEMPLATE, SCENE_HEIGHT,
                     SCENE_WIDTH, UI_HEIGHT, UI_WIDTH, compile_patches,
                     render_size, resolution_writes, scaled_sizes, effect_patches,
                     validate_patch_requirements, external_patches, external_selection,
                     compile_external)

XML = ROOT / 'patches/Bloodborne.xml'
SEGMENTS = [(0, 0x6000000)]


def immediate(writes, address):
    # The XML splits a 9-byte MOV imm32 + NOP into 4+4+1 byte writes.
    data = bytearray(9)
    for offset, value in writes:
        if address <= offset < address + 9:
            data[offset-address:offset-address+len(value)] = value
    assert data[5:] == b'\x0f\x1f\x40\x00'
    return data[0], struct.unpack_from('<I', data, 1)[0]


class NativeUiTests(unittest.TestCase):
    def test_presets_keep_ui_native(self):
        for preset, expected in [(1, (1280, 720)), (2, (1130, 636)),
                                 (3, (960, 540)), (4, (640, 360))]:
            with self.subTest(preset=preset):
                size = render_size({'upscaler': 'fsr3', 'preset': str(preset)})
                self.assertEqual(size, expected)
                writes = resolution_writes(XML, size, '01.09', SEGMENTS)
                self.assertEqual(immediate(writes, SCENE_WIDTH), (0xB8, size[0]))
                self.assertEqual(immediate(writes, SCENE_HEIGHT), (0xB8, size[1]))
                self.assertEqual(immediate(writes, UI_WIDTH), (0xB8, OUTPUT_SIZE[0]))
                self.assertEqual(immediate(writes, UI_HEIGHT), (0xB9, OUTPUT_SIZE[1]))

    def test_explicit_scene_resolution_keeps_native_ui(self):
        writes = resolution_writes(XML, (800, 450), '01.09', SEGMENTS)
        self.assertEqual(immediate(writes, SCENE_WIDTH)[1], 800)
        self.assertEqual(immediate(writes, UI_WIDTH)[1], 1920)
        self.assertEqual(immediate(writes, UI_HEIGHT)[1], 1080)

    def test_coordinate_and_aspect_fixes_are_preserved(self):
        original = compile_patches(XML, [RESOLUTION_TEMPLATE], '01.09', SEGMENTS)
        modified = resolution_writes(XML, (960, 540), '01.09', SEGMENTS)
        self.assertEqual(len(original), len(modified))
        changed = {SCENE_WIDTH, SCENE_HEIGHT, UI_WIDTH, UI_HEIGHT}
        self.assertEqual([w for w in original if w[0] not in changed],
                         [w for w in modified if w[0] not in changed])

    def test_unexpected_or_missing_ui_instruction_is_rejected(self):
        for remove in (False, True):
            tree = ET.parse(XML)
            for meta in tree.getroot().iter('Metadata'):
                if meta.get('Name') == RESOLUTION_TEMPLATE and meta.get('AppVer') == '01.09':
                    patch_list = meta.find('PatchList')
                    for line in list(patch_list):
                        if int(line.get('Address'), 0) == UI_WIDTH + EBOOT_BASE:
                            if remove:
                                patch_list.remove(line)
                            else:
                                line.set('Value', '0x000500B9')
            with tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / 'patch.xml'
                tree.write(path)
                with self.assertRaises(ValueError):
                    resolution_writes(path, (960, 540), '01.09', SEGMENTS)

    def test_native_and_disabled_upscaler_need_no_resolution_patch(self):
        self.assertIsNone(render_size({'preset': '0'}))
        self.assertIsNone(render_size({'upscaler': 'off', 'preset': '3'}))

    def test_output_other_than_1080p_scales_the_scene(self):
        self.assertIsNone(scaled_sizes({'output_res': '1920x1080', 'preset': '2'}))
        # Steam Deck: below 1080p the scene is still the preset's fraction of the output.
        for preset, expected in [(0, (1280, 720)), (2, (752, 424)), (4, (426, 240))]:
            with self.subTest(preset=preset):
                self.assertEqual(scaled_sizes({'output_res': '1280x720', 'preset': str(preset)}),
                                 (expected, (1280, 720)))
        self.assertEqual(scaled_sizes({'output_res': '3840x2160', 'preset': '3'}),
                         ((1916, 1078), (3840, 2160)))
        # TAA is native-only and uses the live host targets.
        self.assertIsNone(scaled_sizes({'output_res': '1280x720', 'upscaler': 'taa', 'preset': '3'}))


class DebugPatchTests(unittest.TestCase):
    def test_camera_patch_is_optional_and_compatible_with_fps_and_debug_menu(self):
        self.assertEqual(effect_patches({'debug_camera': '0', 'debug_menu': '0'}), [])
        camera = effect_patches({'debug_camera': '1'})
        writes = compile_patches(XML, camera, '01.09', SEGMENTS)
        self.assertGreater(len(writes), 0)
        camera_bytes = {offset+i: byte for offset, data in writes for i, byte in enumerate(data)}
        for patch in ('Uncap FPS++', '60 FPS++', '90 FPS++', 'Restore Debug Menu (READ NOTES)'):
            for offset, data in compile_patches(XML, [patch], '01.09', SEGMENTS):
                for i, byte in enumerate(data):
                    if offset+i in camera_bytes:
                        self.assertEqual(camera_bytes[offset+i], byte, patch)

    def test_debug_menu_needs_both_fonts_where_the_game_reads_them(self):
        names = effect_patches({'debug_menu': '1'})
        menu = 'Restore Debug Menu (READ NOTES)'
        with tempfile.TemporaryDirectory() as directory:
            game = Path(directory)
            self.assertEqual(validate_patch_requirements(['Restore Debug Camera'], game),
                             ['Restore Debug Camera'])
            # Without the fonts the patch is left out (the game would crash opening the menu).
            self.assertNotIn(menu, validate_patch_requirements(names, game))
            # The old instructions' folder: still left out, the game reads adhoc/font.
            old = game / 'dvdroot_ps4/font'
            old.mkdir(parents=True)
            (old / 'DbgFont14h.ccm').write_bytes(b'test')
            (old / 'DbgFont14h.tpf').write_bytes(b'test')
            self.assertNotIn(menu, validate_patch_requirements(names, game))
            # A mod's own case (Adhoc/Font/dbgfont14h.*) is found; an empty file is not enough.
            font = game / 'dvdroot_ps4/Adhoc/Font'
            font.mkdir(parents=True)
            (font / 'dbgfont14h.ccm').write_bytes(b'test')
            (font / 'dbgfont14h.tpf').touch()
            self.assertNotIn(menu, validate_patch_requirements(names, game))
            (font / 'dbgfont14h.tpf').write_bytes(b'test')
            self.assertIn(menu, validate_patch_requirements(names, game))

    def test_conflicting_enemy_control_patch_is_rejected(self):
        with self.assertRaisesRegex(ValueError, 'conflicts with Enemy Control'):
            validate_patch_requirements(['Enemy Control', 'Restore Debug Camera'], Path('.'))


EXTERNAL = """<?xml version="1.0"?>
<Patch>
  <TitleID><ID>CUSA03173</ID><ID>CUSA00207</ID></TitleID>
  <Metadata Title="Bloodborne" Name="On" Author="x" PatchVer="1.0" AppVer="01.09" AppElf="eboot.bin" isEnabled="true">
    <PatchList><Line Type="bytes" Address="0x00401000" Value="9090"/></PatchList>
  </Metadata>
  <Metadata Title="Bloodborne" Name="Off" Author="x" PatchVer="1.0" AppVer="01.09" AppElf="eboot.bin">
    <PatchList><Line Type="bytes32" Address="0x00402000" Value="0x12345678"/></PatchList>
  </Metadata>
  <Metadata Title="Bloodborne" Name="Mask" Author="x" PatchVer="1.0" AppVer="01.09" AppElf="eboot.bin" isEnabled="true">
    <PatchList><Line Type="mask" Value="90 ?? 90" Offset="0"/></PatchList>
  </Metadata>
  <Metadata Title="Bloodborne" Name="Old" Author="x" PatchVer="1.0" AppVer="01.00" AppElf="eboot.bin" isEnabled="true">
    <PatchList><Line Type="bytes" Address="0x00403000" Value="90"/></PatchList>
  </Metadata>
</Patch>"""


class ExternalPatchTests(unittest.TestCase):
    def test_selection_and_unsupported_lines(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            (folder / 'extra.xml').write_text(EXTERNAL)
            (folder / 'other.xml').write_text(EXTERNAL.replace('CUSA03173', 'CUSA99999')
                                              .replace('CUSA00207', 'CUSA99998'))
            (folder / 'broken.xml').write_text('<Patch>')
            found = external_patches(folder)
            self.assertEqual([key for key, _, _ in found],
                             ['extra.xml/On', 'extra.xml/Off', 'extra.xml/Mask'])
            # The file's isEnabled; the mask patch is skipped as a whole.
            writes = compile_external(external_selection(found, None), SEGMENTS)
            self.assertEqual(writes, [(0x1000, bytes.fromhex('9090'))])
            config = folder / 'patches.json'
            config.write_text('{"enabled": ["extra.xml/Off"], "disabled": ["extra.xml/On"]}')
            writes = compile_external(external_selection(found, config), SEGMENTS)
            self.assertEqual(writes, [(0x2000, (0x12345678).to_bytes(4, 'little'))])

    def test_built_in_file_is_not_external(self):
        self.assertEqual(external_patches(XML.parent), [])


class IntelTonemapTests(unittest.TestCase):
    def cpuinfo(self, vendor):
        path = Path(tempfile.mkdtemp()) / 'cpuinfo'
        path.write_text(f'processor\t: 0\nvendor_id\t: {vendor}\nmodel name\t: x\n')
        return str(path)

    def test_on_for_intel_off_for_amd(self):
        self.assertTrue(patches.intel_tonemap_fix({}, self.cpuinfo('GenuineIntel')))
        self.assertFalse(patches.intel_tonemap_fix({}, self.cpuinfo('AuthenticAMD')))

    def test_environment_forces_it(self):
        amd, intel = self.cpuinfo('AuthenticAMD'), self.cpuinfo('GenuineIntel')
        self.assertTrue(patches.intel_tonemap_fix({'BB_INTEL_TONEMAP_FIX': '1'}, amd))
        self.assertFalse(patches.intel_tonemap_fix({'BB_INTEL_TONEMAP_FIX': '0'}, intel))

    def test_patch_exists_for_109(self):
        xml = ET.parse(ROOT / 'patches/Bloodborne.xml')
        names = [m.get('Name') for m in xml.iter('Metadata')]
        self.assertIn(patches.INTEL_TONEMAP, names)


class PartyPatchTests(unittest.TestCase):
    def test_seamless_patches_follow_bb_party(self):
        self.assertEqual(patches.party_patches({}), [])
        self.assertEqual(patches.party_patches({'BB_PARTY': 'host'}), patches.PARTY_SEAMLESS + [patches.PARTY_NO_INSIGHT])
        self.assertEqual(patches.party_patches({'BB_PARTY': 'host', 'BB_PARTY_BELL_NO_INSIGHT': '0'}), patches.PARTY_SEAMLESS)
        self.assertEqual(patches.party_patches({'BB_PARTY': 'host', 'BB_PARTY_SEAMLESS': '0'}), [])

    def test_network_choice(self):
        both = {patches.SKIP_NETWORK_CHOICE, patches.SKIP_NETWORK_CHOICE_ONLINE}
        self.assertEqual(patches.network_choice_patch({}, both), patches.SKIP_NETWORK_CHOICE)
        self.assertIsNone(patches.network_choice_patch({'BB_SKIP_NETWORK_CHOICE': '0'}, both))
        self.assertEqual(patches.network_choice_patch({'BB_SKIP_NETWORK_CHOICE': 'online'}, both),
                         patches.SKIP_NETWORK_CHOICE_ONLINE)
        self.assertIsNone(patches.network_choice_patch({'BB_SKIP_NETWORK_CHOICE': 'online'},
                                                       {patches.SKIP_NETWORK_CHOICE}))

    def test_party_patches_exist_compile_and_carry_originals(self):
        names = patches.PARTY_SEAMLESS + [patches.PARTY_NO_INSIGHT, patches.SKIP_NETWORK_CHOICE_ONLINE]
        writes = compile_patches(XML, names, '01.09', SEGMENTS)
        self.assertTrue(writes)
        for meta in ET.parse(XML).getroot().iter('Metadata'):
            if meta.get('Name') in names:
                self.assertEqual(meta.get('isEnabled'), 'false')
                for line in meta.iter('Line'):
                    self.assertEqual(len(bytes.fromhex(line.get('Original'))), len(patches.encode(line)))

    def test_original_mismatch_leaves_the_patch_out(self):
        # A fake ELF: one PT_LOAD segment, vaddr 0 at file offset 0x1000.
        size = 0x1a00000
        elf = bytearray(0x1000 + size)
        struct.pack_into('<Q', elf, 0x20, 0x40)
        struct.pack_into('<HH', elf, 0x36, 0x38, 1)
        struct.pack_into('<IIQQQQQQ', elf, 0x40, 1, 5, 0x1000, 0, 0, size, size, 0x1000)
        name = 'Party: Keep session on map reload'
        self.assertFalse(patches.originals_match(XML, name, '01.09', bytes(elf)))
        elf[0x1000 + 0x19471b1:0x1000 + 0x19471b6] = bytes.fromhex('e8ea955800')
        self.assertTrue(patches.originals_match(XML, name, '01.09', bytes(elf)))


class PartyVersionCheckTests(unittest.TestCase):
    """out/party_patch_hash.txt: only gameplay patches count in the party's version check."""

    def all_names(self):
        return [m.get('Name') for m in ET.parse(XML).getroot().iter('Metadata') if m.get('AppVer') == '01.09']

    def test_classification_of_the_built_in_patches(self):
        names = self.all_names()
        for name in patches.COSMETIC_PATCHES:
            self.assertIn(name, names)  # no stale entries
        cosmetic = {n for n in names if patches.patch_is_cosmetic(n)}
        gameplay = set(names) - cosmetic
        # Party patches and the skip-online dialog always count; so do cheats and rally changes.
        for name in [*patches.PARTY_SEAMLESS, patches.PARTY_NO_INSIGHT, patches.SKIP_NETWORK_CHOICE,
                     patches.SKIP_NETWORK_CHOICE_ONLINE, 'Player No Dead (Read note)', 'Disable Rally (HP Regain)',
                     'No Rally Decay', 'Enemy Control', 'DS1-like physics', '60FPS (no deltatime)',
                     patches.DEBUG_MENU]:
            self.assertIn(name, gameplay)
        # Frame rate, resolution, effects, LOD and the platform workarounds never do.
        for name in [*{n for preset in patches.FPS_PRESETS.values() for n in preset}, patches.RESOLUTION_TEMPLATE,
                     'Resolution Patch 3840x2160 (16:9)', '4k Light Grid (READ NOTES)', 'Increased Graphics Heap Sizes',
                     patches.INTEL_TONEMAP, *patches.MODEL_LOD.values()]:
            self.assertIn(name, cosmetic)
        for off, on in patches.EFFECTS.values():
            for name in (off, on):
                if name and name not in (patches.DEBUG_MENU, 'Restore Debug Camera'):
                    self.assertIn(name, cosmetic)

    def items(self, names):
        metas = {m.get('Name'): m for m in ET.parse(XML).getroot().iter('Metadata') if m.get('AppVer') == '01.09'}
        return [(n, metas[n], compile_patches(XML, [n], '01.09', SEGMENTS)) for n in names]

    def test_hash_ignores_cosmetic_patches_and_order(self):
        gameplay = patches.PARTY_SEAMLESS + [patches.SKIP_NETWORK_CHOICE_ONLINE]
        base, names = patches.party_patch_set(self.items(gameplay))
        self.assertEqual(names, sorted(gameplay))
        graphics = ['Uncap FPS++', 'Sprint Fix (High FPS)', 'Disable Motion Blur (perf increase)',
                    'Resolution Patch 2560x1440 (16:9)', 'Model LOD 2 (Lowest)', patches.INTEL_TONEMAP]
        self.assertEqual(patches.party_patch_set(self.items(graphics + gameplay[::-1]))[0], base)
        self.assertEqual(patches.party_patch_set(self.items(['60 FPS++'] + gameplay))[0], base)
        cheat, names = patches.party_patch_set(self.items(gameplay + ['Player No Dead (Read note)']))
        self.assertNotEqual(cheat, base)
        self.assertIn('Player No Dead (Read note)', names)
        self.assertNotEqual(patches.party_patch_set(self.items(gameplay[1:]))[0], base)
        # Same name, other bytes (another version of the patch file): another hash.
        name, meta, writes = self.items(gameplay[:1])[0]
        changed = [(name, meta, [(writes[0][0], bytes(len(writes[0][1])))] + writes[1:])]
        self.assertNotEqual(patches.party_patch_set(changed)[0], patches.party_patch_set([(name, meta, writes)])[0])
        # Nothing gameplay-related at all: a fixed hash, no names.
        empty, names = patches.party_patch_set(self.items(graphics))
        self.assertEqual(names, [])
        self.assertEqual(empty, patches.party_patch_set([])[0])

    def test_external_patch_marked_cosmetic(self):
        meta = ET.fromstring('<Metadata Name="My Reshade-ish tweak" Party="cosmetic"/>')
        self.assertTrue(patches.patch_is_cosmetic('My Reshade-ish tweak', meta))
        self.assertFalse(patches.patch_is_cosmetic('My Reshade-ish tweak', ET.fromstring('<Metadata Name="x"/>')))
        # An XML can also insist that a listed name counts.
        self.assertFalse(patches.patch_is_cosmetic('60 FPS++', ET.fromstring('<Metadata Party="gameplay"/>')))

    def test_main_writes_the_party_file(self):
        """patches.py end to end: 60 FPS vs uncapped, same gameplay hash; a cheat changes it."""
        import os
        import subprocess
        import sys
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            # A tiny ELF whose one PT_LOAD covers every patch address in memory (no file bytes:
            # patches with Original= are left out, the same way on every run).
            elf = bytearray(0x200)
            struct.pack_into('<Q', elf, 0x20, 0x40)
            struct.pack_into('<HH', elf, 0x36, 0x38, 1)
            struct.pack_into('<IIQQQQQQ', elf, 0x40, 1, 5, 0x100, 0, 0, 0, 0x6000000, 0x1000)
            (out / 'eboot.elf').write_bytes(bytes(elf))
            env = {**os.environ, 'BB_PARTY': 'host', 'BB_INTEL_TONEMAP_FIX': '0', 'BB_SKIP_NETWORK_CHOICE': '1'}

            def run(fps, extra=''):
                subprocess.run([sys.executable, str(ROOT / 'scripts/patches.py'), '--out', str(out), '--fps', fps,
                                '--extra', extra, '--settings', str(out / 'none.ini'), '--game-dir', str(out)],
                               check=True, env=env, capture_output=True)
                text = (out / patches.PARTY_PATCH_FILE).read_text(encoding='utf-8')
                return text, (out / 'patches.bin').read_bytes()

            fast, fast_bin = run('uncap')
            slow, slow_bin = run('60')
            self.assertNotEqual(fast_bin, slow_bin)
            hash_line = [line for line in fast.splitlines() if line.startswith('hash ')]
            self.assertEqual(len(hash_line), 1)
            self.assertEqual(len(hash_line[0].split()[1]), 64)
            self.assertEqual(hash_line, [line for line in slow.splitlines() if line.startswith('hash ')])
            self.assertIn(f'patch {patches.SKIP_NETWORK_CHOICE}', fast.splitlines())
            self.assertIn('# cosmetic Uncap FPS++', fast.splitlines())
            cheat, _ = run('60', 'Player No Dead (Read note)')
            self.assertIn('patch Player No Dead (Read note)', cheat.splitlines())
            self.assertNotEqual([line for line in cheat.splitlines() if line.startswith('hash ')], hash_line)


if __name__ == '__main__':
    unittest.main()
