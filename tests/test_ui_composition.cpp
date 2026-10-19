// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cmath>
#include <cstdio>
#include "gpu/shadps4/video_core/renderer_vulkan/ui_composition.h"

int main() {
    using namespace Vulkan::UiComposition;
    // Title/loading menus have no depth/camera. UI must still go to the native target.
    assert(Choose(true, true, false, true) == Background::Copy);
    assert(Choose(true, true, false, false) == Background::Copy);
    // In-game HUD: first reconstruct the scene, then draw the native UI over it.
    assert(Choose(true, true, true, true) == Background::Temporal);
    // Runtime FSR disable/failure must not change UI resolution.
    assert(Choose(true, true, true, false) == Background::Copy);
    assert(Choose(true, false, true, true) == Background::None);
    assert(Choose(false, true, true, true) == Background::None);
    assert(NativeViewport(1920, -1080));
    assert(!NativeViewport(960, -540));
    assert(MovieShader(0x34e8a281) && MovieShader(0x24042a9b));
    assert(!MovieShader(0x0b0acf50)); // fullscreen post/tonemap also has a native viewport
    assert(!MovieShader(0x6c62a79f)); // shadow geometry
    const auto display = Scale(960, 540, 1920, 1080, false);
    assert(display[0] == 2 && display[1] == 2);
    const auto ui = Scale(960, 540, 1920, 1080, true);
    assert(ui[0] == 1 && ui[1] == 1); // don't double an already native UI viewport
    const auto larger = Scale(960, 540, 3840, 2160, true);
    assert(larger[0] == 2 && larger[1] == 2);
    // Ultrawide stage: black side columns only for a menu without a scene (21:9, 32:9); a frame
    // with a scene (the world with the upscaler off) keeps its whole width; 16:9 has no sides.
    assert(StageSide(false, 1920) == 0.0f && StageSide(true, 1920) == 0.0f);
    assert(StageSide(true, 2560) == 0.0f && StageSide(true, 3840) == 0.0f);
    assert(std::fabs(StageSide(false, 2560) - 0.125f) < 1e-6f);
    assert(std::fabs(StageSide(false, 3840) - 0.25f) < 1e-6f);
    // Supersampling (render above the output) skips the temporal upscalers; equal or below runs.
    assert(SceneAboveOutput(2560, 1440, 1920, 1080));
    assert(SceneAboveOutput(1920, 1440, 1920, 1080) && SceneAboveOutput(2560, 1080, 1920, 1080));
    assert(!SceneAboveOutput(1920, 1080, 1920, 1080) && !SceneAboveOutput(1280, 720, 1920, 1080));
    std::puts("UI composition: PASS (menu without camera, HUD, FSR off, viewport scaling, "
              "ultrawide stage, scene above the output)");
}
