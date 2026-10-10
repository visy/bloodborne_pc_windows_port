// SPDX-License-Identifier: GPL-3.0-or-later
// bbport: the effect ribbons' last-point writers without the red zone (from droogie/bbhost,
// GPL-3.0-or-later; see bbport_ribbons.cpp). Installed over the game's by bbport_stability.cpp.
#pragma once

#include <atomic>
#include <cstdint>

// The game's calling convention (System V), said explicitly for Windows.
#define BB_RIBBON_ABI __attribute__((sysv_abi))

namespace BbRibbons {
/// Calls through ours (for the exit / periodic report).
extern std::atomic<std::uint64_t> facing_calls, along_calls;

/// guest +0x28ce7b0 (Binary Ninja 0x2cce7b0): a strip facing the eye.
BB_RIBBON_ABI void Facing(std::uint8_t* out, std::uint32_t n, const float* born, const float* px, const float* py,
                          const float* pz, float now, float eye_x, float eye_y, float eye_z, float life, float v,
                          float half_width, float u0, const std::uint32_t* packed, const float* u_offset,
                          float u_span, float red, float green, float blue, float opacity, std::uint32_t first,
                          std::uint32_t total, float fade_in, float fade_out);

/// guest +0x28ceec0 (Binary Ninja 0x2cceec0): a strip along the points' normals.
BB_RIBBON_ABI void Along(std::uint8_t* out, std::uint32_t n, const float* born, const float* px, const float* py,
                         const float* pz, float now, float life, float v, float half_width, float u0, float u_span,
                         float red, float green, const std::uint32_t* packed, const float* u_offset, const float* nx,
                         const float* ny, const float* nz, std::uint32_t per_point, float blue, float opacity,
                         std::uint32_t first, std::uint32_t total, float fade_in, float fade_out);
} // namespace BbRibbons
