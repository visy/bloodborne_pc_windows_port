// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: native HDR output (gpu/shim/bbport_hdr.h, I4): PQ encoding, the Rec.709 -> Rec.2020
// conversion, the inverse tone curve (mid-tones kept, highlights expanded without hue shifts),
// the peak limit, the overlay at paper white, and the settings (bbport.ini / environment).
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include "gpu/shim/bbport_hdr.h"
#include "gpu/shim/bbport_settings.h"

using namespace BbHdr;

static bool Near(float a, float b, float tolerance) {
    return std::fabs(a - b) <= tolerance;
}

static void PqCurve() {
    assert(Near(PqEncode(0.0f), 0.0f, 1e-5f));
    assert(Near(PqEncode(10000.0f), 1.0f, 1e-6f));
    // Reference values of ST 2084 (ITU-R BT.2100 / BT.2408 tables).
    assert(Near(PqEncode(100.0f), 0.5081f, 5e-4f));
    assert(Near(PqEncode(203.0f), 0.5806f, 5e-4f)); // BT.2408 HDR reference white
    assert(Near(PqEncode(1000.0f), 0.7518f, 5e-4f));
    assert(Near(PqEncode(4000.0f), 0.9026f, 5e-4f));
    for (float nits : {0.01f, 0.5f, 5.0f, 80.0f, 200.0f, 640.0f, 1000.0f, 3000.0f, 9000.0f}) {
        assert(Near(PqDecode(PqEncode(nits)), nits, nits * 1e-3f + 1e-4f));
    }
    // Monotonic, and a 10-bit code is fine enough around paper white (< 1 % a step).
    float last = -1.0f;
    for (int i = 0; i <= 1000; ++i) {
        const float e = PqEncode(float(i) * 10.0f);
        assert(e > last);
        last = e;
    }
    const float code = std::round(PqEncode(200.0f) * 1023.0f);
    assert(PqDecode((code + 1) / 1023.0f) / PqDecode(code / 1023.0f) < 1.01f);
}

static void Gamut() {
    const Rgb white = Rec709To2020({1, 1, 1});
    assert(Near(white.r, 1, 1e-4f) && Near(white.g, 1, 1e-4f) && Near(white.b, 1, 1e-4f));
    for (Rgb c : {Rgb{1, 0, 0}, Rgb{0, 1, 0}, Rgb{0, 0, 1}, Rgb{0.3f, 0.7f, 0.2f}}) {
        const Rgb back = Rec2020To709(Rec709To2020(c));
        assert(Near(back.r, c.r, 1e-4f) && Near(back.g, c.g, 1e-4f) && Near(back.b, c.b, 1e-4f));
        // Rec.709 primaries lie inside Rec.2020: no negative component.
        const Rgb wide = Rec709To2020(c);
        assert(wide.r >= 0 && wide.g >= 0 && wide.b >= 0);
    }
    // Luminance is kept (Y of Rec.2020 with its own coefficients).
    const Rgb red = Rec709To2020({1, 0, 0});
    assert(Near(0.2627f * red.r + 0.6780f * red.g + 0.0593f * red.b, 0.2126f, 2e-3f));
}

static void Curve() {
    const Params p{1000.0f, 200.0f, 1.0f, 1.0f};
    // Below the knee: the identity (mid-tones at paper white).
    for (float y : {0.0f, 0.01f, 0.18f, 0.3f, Knee}) assert(Expand(y, p) == y);
    // SDR white reaches the peak; continuous with slope 1 at the knee; increasing.
    assert(Near(Expand(1.0f, p), 5.0f, 1e-5f));
    const float d = 1e-3f;
    assert(Near((Expand(Knee + d, p) - Expand(Knee, p)) / d, 1.0f, 0.02f));
    float last = 0.0f;
    for (int i = 1; i <= 1000; ++i) {
        const float v = Expand(float(i) / 1000.0f, p);
        assert(v > last);
        last = v;
    }
    // Strength 0: the SDR picture as it is; 0.5: half way.
    assert(Expand(1.0f, {1000.0f, 200.0f, 0.0f, 1.0f}) == 1.0f);
    assert(Near(Expand(1.0f, {1000.0f, 200.0f, 0.5f, 1.0f}), 3.0f, 1e-5f));
    // Peak below paper white: no expansion.
    assert(Near(WhiteTarget({150.0f, 200.0f, 1.0f, 1.0f}), 1.0f, 1e-6f));

    // Mid grey (encoded 0.5) stays at 0.5^2.2 * paper white, in every channel.
    const Rgb grey = EncodePq({0.5f, 0.5f, 0.5f}, p);
    const float grey_nits = std::pow(0.5f, 2.2f) * 200.0f;
    assert(Near(PqDecode(grey.r), grey_nits, 0.05f) && Near(PqDecode(grey.b), grey_nits, 0.05f));
    // White at the peak.
    const Rgb white = EncodePq({1, 1, 1}, p);
    assert(Near(PqDecode(white.g), 1000.0f, 1.0f) && Near(PqDecode(white.r), 1000.0f, 1.0f));
    // Black stays black.
    assert(Near(PqDecode(EncodePq({0, 0, 0}, p).r), 0.0f, 1e-3f));
}

static void Hue() {
    const Params p{1000.0f, 200.0f, 1.0f, 1.0f};
    // A warm highlight (lamp light): its chromaticity (channel ratios) is kept by the expansion.
    const Rgb in{1.0f, 0.85f, 0.6f};
    const Rgb lin{SdrDecode(in.r), SdrDecode(in.g), SdrDecode(in.b)};
    const Rgb out = ExpandColor(in, p);
    assert(Luminance709(out) > Luminance709(lin) * 2.0f); // expanded
    assert(Near(out.g / out.r, lin.g / lin.r, 1e-4f) && Near(out.b / out.r, lin.b / lin.r, 1e-4f));
    // Through the peak limit too: a colour over the peak is scaled, not clipped per channel.
    const Rgb nits = ToRec2020Nits({9.0f, 4.0f, 1.0f}, p);
    const Rgb wide = Rec709To2020({9.0f, 4.0f, 1.0f});
    assert(Near(std::max({nits.r, nits.g, nits.b}), 1000.0f, 0.5f));
    assert(Near(nits.g / nits.r, wide.g / wide.r, 1e-4f) && Near(nits.b / nits.r, wide.b / wide.r, 1e-4f));
    // Saturation 1 changes nothing; 0.5 moves towards grey at the same luminance.
    const Rgb half = ExpandColor({0.8f, 0.3f, 0.2f}, {1000.0f, 200.0f, 1.0f, 0.5f});
    const Rgb full = ExpandColor({0.8f, 0.3f, 0.2f}, p);
    assert(Near(Luminance709(half), Luminance709(full), 1e-5f));
    assert(half.r < full.r && half.b > full.b);
}

static void Overlay() {
    const Rgb bg{2.0f, 1.0f, 0.5f};
    // Nothing drawn: the background as it is.
    const Rgb none = BlendOverlay(bg, {0, 0, 0}, 0.0f);
    assert(none.r == bg.r && none.g == bg.g && none.b == bg.b);
    // Opaque white text: exactly paper white, whatever the highlight under it.
    const Rgb text = BlendOverlay(bg, {1, 1, 1}, 1.0f);
    assert(Near(text.r, 1.0f, 1e-5f) && Near(text.g, 1.0f, 1e-5f));
    const Rgb nits = ToRec2020Nits(text, {1000.0f, 200.0f, 1.0f, 1.0f});
    assert(Near(nits.r, 200.0f, 0.05f) && Near(nits.b, 200.0f, 0.05f));
    // A 50 % black panel over paper white: as an SDR swapchain blends it (encoded 0.5).
    const Rgb panel = BlendOverlay({1, 1, 1}, {0, 0, 0}, 0.5f);
    assert(Near(panel.g, std::pow(0.5f, 2.2f), 1e-5f));
}

static void Peak() {
    assert(ResolvePeak(0, 0.0f) == DefaultPeakNits);
    assert(ResolvePeak(0, 650.0f) == 650.0f);
    assert(ResolvePeak(1500, 650.0f) == 1500.0f);
    assert(ResolvePeak(20000, 0.0f) == MaxPeakNits);
    assert(ResolvePeak(0, 40.0f) == MinPeakNits);
}

static void Settings() {
    const char* path = "hdr_test_bbport.ini";
    {
        std::ofstream ini(path);
        ini << "hdr=1\nhdr_peak_nits=1500\nhdr_paper_white=180\nhdr_strength=0.80\nhdr_saturation=1.10\n";
    }
#ifdef _WIN32
    _putenv_s("BB_CONFIG", path);
#else
    setenv("BB_CONFIG", path, 1);
#endif
    BbSettings::Load();
    auto& s = BbSettings::Get();
    assert(s.hdr && s.startup_hdr && s.hdr_peak_nits == 1500 && s.hdr_paper_white == 180);
    assert(Near(s.hdr_strength, 0.8f, 1e-6f) && Near(s.hdr_saturation, 1.1f, 1e-6f));
    assert(BbSettings::HdrPeakNits() == 1500.0f);
    assert(!s.hdr_calibration); // never from the file
    // Automatic peak: the display's report, else 1000.
    s.hdr_peak_nits = 0;
    assert(BbSettings::HdrPeakNits() == 1000.0f);
    s.hdr_display_peak = 720.0f;
    assert(BbSettings::HdrPeakNits() == 720.0f);
    // Off <-> on is a restart setting.
    assert(!BbSettings::RestartNeeded());
    s.hdr = false;
    assert(BbSettings::RestartNeeded());
    s.hdr = true;
    BbSettings::Save();
    std::ifstream in(path);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    assert(text.find("hdr=1\n") != std::string::npos);
    assert(text.find("hdr_peak_nits=auto\n") != std::string::npos);
    assert(text.find("hdr_paper_white=180\n") != std::string::npos);
    assert(text.find("hdr_calibration") == std::string::npos);
    // Environment overrides (BB_HDR, BB_HDR_PEAK, BB_HDR_PAPER_WHITE) and clamping.
#ifdef _WIN32
    _putenv_s("BB_HDR", "0");
    _putenv_s("BB_HDR_PEAK", "50000");
    _putenv_s("BB_HDR_PAPER_WHITE", "20");
#else
    setenv("BB_HDR", "0", 1);
    setenv("BB_HDR_PEAK", "50000", 1);
    setenv("BB_HDR_PAPER_WHITE", "20", 1);
#endif
    BbSettings::Load();
    assert(!s.hdr && !s.startup_hdr && s.hdr_peak_nits == 10000 && s.hdr_paper_white == 80);
    std::remove(path);
}

int main() {
    PqCurve();
    Gamut();
    Curve();
    Hue();
    Overlay();
    Peak();
    Settings();
    std::puts("HDR output: PASS (PQ, Rec.2020, inverse tone curve, hue, peak limit, overlay at paper white, settings)");
}
