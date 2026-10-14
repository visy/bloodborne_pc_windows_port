// SPDX-License-Identifier: GPL-2.0-or-later
// Frame generation settings (from Supermedo/bloodborne_pc PR #69 by FIocker, adapted): off by
// default, bbport.ini / environment round trip, FSR clamped to 2x, a restart needed on a change.
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include "gpu/shim/bbport_settings.h"
#include "test_portable.h"

static void WriteIni(const char* path, const char* text) {
    const int written = bb_test_write_file(path, text, std::strlen(text));
    assert(written == 0);
}

static std::string Read(const char* path) {
    std::ifstream file(path);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

int main() {
    using namespace BbSettings;
    char path[1024];
    const int created = bb_test_temp_file(path, sizeof(path), "bbport-fg-settings-test-");
    assert(created == 0);
    int env = bb_test_setenv("BB_CONFIG", path);
    assert(env == 0);
    auto& s = Get();

    // Off by default: an empty bbport.ini.
    WriteIni(path, "# empty\n");
    Load();
    assert(s.frame_generation == 0 && s.fg_backend == FgDlss);
    assert(s.startup_frame_generation == 0 && !FrameGenerationStarted());
    assert(!RestartNeeded());

    // DLSS 2x / 3x / 4x round trip through Save.
    for (int factor : {2, 3, 4}) {
        char text[128];
        std::snprintf(text, sizeof(text), "fg_backend=dlss\nframe_generation=%dx\n", factor);
        WriteIni(path, text);
        Load();
        assert(s.frame_generation == factor && s.fg_backend == FgDlss && FrameGenerationStarted());
        Save();
        const std::string saved = Read(path);
        assert(saved.find(std::string("frame_generation=") + FrameGenName(factor)) != std::string::npos);
        s.frame_generation = 0;
        Load();
        assert(s.frame_generation == factor);
    }

    // FSR supports 2x only: 3x / 4x clamp, in either key order.
    WriteIni(path, "fg_backend=fsr\nframe_generation=4x\n");
    Load();
    assert(s.fg_backend == FgFsr && s.frame_generation == 2);
    WriteIni(path, "frame_generation=3x\nfg_backend=fsr\n");
    Load();
    assert(s.fg_backend == FgFsr && s.frame_generation == 2);

    // A key dropped from the file means off again (no carry-over from the previous Load).
    WriteIni(path, "upscaler=fsr3\n");
    Load();
    assert(s.frame_generation == 0 && s.fg_backend == FgDlss);

    // The environment overrides the file.
    WriteIni(path, "frame_generation=off\n");
    env = bb_test_setenv("BB_FRAME_GENERATION", "2x");
    assert(env == 0);
    env = bb_test_setenv("BB_FG_BACKEND", "fsr");
    assert(env == 0);
    Load();
    assert(s.frame_generation == 2 && s.fg_backend == FgFsr && s.startup_fg_backend == FgFsr);
    bb_test_setenv("BB_FRAME_GENERATION", "off");
    bb_test_setenv("BB_FG_BACKEND", "dlss");
    Load();
    assert(s.frame_generation == 0 && !FrameGenerationStarted());

    // A menu change applies after a restart.
    s.frame_generation = 2;
    assert(RestartNeeded());
    s.frame_generation = 0;
    assert(!RestartNeeded());

    assert(ParseFrameGen("bad") == 0 && ParseFrameGen("4") == 4 && ParseFgBackend("x") == FgDlss);
    std::remove(path);
    std::puts("PASS: frame generation settings");
}
