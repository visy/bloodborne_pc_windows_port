"""Ultrawide outputs (from Supermedo/bloodborne_pc PR #70 by emurphy04)."""
from paths import ROOT
import struct
import unittest

from patches import (CAMERA_ASPECT, EBOOT_BASE, HUD_PROJECTION_WIDTH, MARKER_BIAS, OUTPUT_SIZE,
                     ULTRAWIDE_TEMPLATE, compile_patches, hud_canvas, ultrawide_writes)

XML = ROOT / 'patches/Bloodborne.xml'
SEGMENTS = [(0, 0x6000000)]


def apply(writes):
    image = {}
    for offset, data in writes:
        for i, byte in enumerate(data):
            image[offset + i] = byte
    return image


def value(image, offset, size=4):
    return bytes(image[offset + i] for i in range(size))


class UltrawideTests(unittest.TestCase):
    def test_canvas(self):
        self.assertEqual(hud_canvas((3440, 1440)), (2580, 1080))
        self.assertEqual(hud_canvas((2560, 1080)), (2560, 1080))
        # 16:9 and narrower keep the game's own coordinates.
        self.assertEqual(hud_canvas((3840, 2160)), OUTPUT_SIZE)
        self.assertEqual(hud_canvas((1920, 1200)), OUTPUT_SIZE)

    def test_2560x1080_matches_the_community_patch(self):
        ours = apply(ultrawide_writes(XML, (2560, 1080), '01.09', SEGMENTS))
        theirs = apply(compile_patches(XML, [ULTRAWIDE_TEMPLATE], '01.09', SEGMENTS))
        # Scene and UI viewport sizes come from resolution_writes instead.
        for offset, byte in ours.items():
            self.assertEqual(theirs[offset], byte, hex(offset + EBOOT_BASE))

    def test_3440x1440_values(self):
        image = apply(ultrawide_writes(XML, (3440, 1440), '01.09', SEGMENTS))
        aspect, = struct.unpack('<f', value(image, CAMERA_ASPECT))
        width, = struct.unpack('<f', value(image, HUD_PROJECTION_WIDTH))
        self.assertAlmostEqual(aspect, 3440 / 1440, places=5)
        self.assertEqual(width, 2580.0)
        for site in MARKER_BIAS:
            bias, = struct.unpack('<f', value(image, site))
            self.assertAlmostEqual(bias, 0.5 * 1920 / 2580, places=6)

    def test_scaled_sizes_and_print(self):
        from patches import scaled_sizes
        # A native-size ultrawide scene would look like the HUD canvas: drawn 4x2 smaller.
        self.assertEqual(scaled_sizes({'output_res': '2560x1080', 'upscaler': 'off'}),
                         ((2556, 1078), (2560, 1080)))
        self.assertEqual(scaled_sizes({'output_res': '3440x1440', 'upscaler': 'fsr3', 'preset': '0'}),
                         ((3440, 1440), (3440, 1440)))


if __name__ == '__main__':
    unittest.main()
