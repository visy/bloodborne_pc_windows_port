// SPDX-License-Identifier: GPL-3.0-or-later
// Seamless party rules (plan phase A6): what the party layer changes in the game besides the
// "Party: ..." byte patches of patches/Bloodborne.xml (which scripts/patches.py enables with
// BB_PARTY set and BB_PARTY_SEAMLESS != 0; the loader writes them before the game runs).
//
// SeamlessRulesInit (bbgpu_patch_image, after HooksInit, before the game runs):
//   - reports, per XML party patch site, whether the image holds the patched bytes ("applied"),
//     the 1.09 original ("off") or something else ("MISMATCH");
//   - installs the EMEVD instruction dispatch filter (0x17b93a0) when BB_PARTY_EMEVD_TRACE=1 or
//     a skip rule exists (BB_PARTY_EMEVD_SKIP or the built-in table, empty for now).
// SeamlessRulesTick (the main-thread coop tick, once a frame): param writes once the game's
// params are loaded, re-checked every 2 s (a param reload would restore the game's values):
//   - SpEffectParam 9006 (cooperator) and 9026 (invader phantom) maxHpRate 0.7 -> 1.0
//   - EquipParamGoods 200 (Beckoning Bell) consumeHeroPoint 1 -> 0: no Insight needed or spent
//   - opt-in (BB_PARTY_GUEST_MARK=1): EquipParamGoods 100 / 1400 (Hunter's Marks) byte 0x44 gets
//     enable_white | enable_multi, so guests can use them (party_phantom.h)
//
// Env: BB_PARTY_SEAMLESS=0 turns all of it off (and patches.py leaves the XML patches out);
//      BB_PARTY_FULL_HP=0, BB_PARTY_BELL_NO_INSIGHT=0 turn off a single param rule;
//      BB_PARTY_EMEVD_TRACE=1 logs each new (event, bank, instruction) the game executes;
//      BB_PARTY_EMEVD_SKIP="event:bank:id[@index][,...]" ("*" = any event; @index = only that
//      instruction index in the event) skips those instructions. Built in (party mode, B1 travel
//      on, BB_PARTY_OPEN_WORLD not 0): 7600:2005:3@6, 7600:2006:2@7 (confinement walls). The
//      filter also feeds party_phantom's boss-Insight observer (BB_PARTY_GUEST_INSIGHT not 0).
#pragma once

namespace coop {

/// BB_PARTY set and BB_PARTY_SEAMLESS is not "0".
bool SeamlessRequested();
/// Call once from PartyInit after HooksInit (only when the party layer is on).
void SeamlessRulesInit();
/// Call once a frame from the main-thread coop tick.
void SeamlessRulesTick();

} // namespace coop
