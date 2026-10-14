// SPDX-License-Identifier: GPL-3.0-or-later
// Party balance rules, the pure part (see party_balance_rules.h).
#include "party_balance_rules.h"

#include "../net/json.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace coop::balance {

const int kHpTable[11] = {0, 25, 50, 75, 100, 125, 150, 200, 250, 300, 400};
const int kDamageTable[11] = {0, 5, 10, 15, 20, 25, 30, 40, 50, 75, 100};
const int kPoiseTable[11] = {-100, -75, -50, -25, 0, 25, 50, 100, 150, 200, 300};

bool Rules::operator==(const Rules& o) const {
    return rot == o.rot && boss_hp == o.boss_hp && boss_damage == o.boss_damage && boss_poise == o.boss_poise &&
           enemy_hp == o.enemy_hp && enemy_damage == o.enemy_damage && enemy_poise == o.enemy_poise;
}

bool Doping::operator==(const Doping& o) const {
    return hp_rate == o.hp_rate && resist_rate == o.resist_rate && attack_rate == o.attack_rate &&
           super_armor == o.super_armor;
}

Rules Vanilla() {
    Rules r;
    r.rot = false;
    r.boss_hp = 100;
    r.boss_damage = 0;
    r.boss_poise = 100;
    r.enemy_hp = 0;
    r.enemy_damage = 0;
    r.enemy_poise = 0;
    return r;
}

int Position(const int* table, int value) {
    int best = 0;
    for (int i = 1; i < 11; ++i) {
        if (std::abs(table[i] - value) < std::abs(table[best] - value)) best = i;
    }
    return best;
}

int AtPosition(const int* table, int position) {
    return table[std::clamp(position, 0, 10)];
}

Rules Clamped(const Rules& r) {
    Rules c = r;
    c.boss_hp = std::clamp(r.boss_hp, kHpTable[0], kHpTable[10]);
    c.enemy_hp = std::clamp(r.enemy_hp, kHpTable[0], kHpTable[10]);
    c.boss_damage = std::clamp(r.boss_damage, kDamageTable[0], kDamageTable[10]);
    c.enemy_damage = std::clamp(r.enemy_damage, kDamageTable[0], kDamageTable[10]);
    c.boss_poise = std::clamp(r.boss_poise, kPoiseTable[0], kPoiseTable[10]);
    c.enemy_poise = std::clamp(r.enemy_poise, kPoiseTable[0], kPoiseTable[10]);
    return c;
}

std::string ToJson(const Rules& r) {
    json::Value v = json::Value::make_object();
    v.set("rot", r.rot);
    v.set("boss_hp", r.boss_hp);
    v.set("boss_damage", r.boss_damage);
    v.set("boss_poise", r.boss_poise);
    v.set("enemy_hp", r.enemy_hp);
    v.set("enemy_damage", r.enemy_damage);
    v.set("enemy_poise", r.enemy_poise);
    return json::dump(v, 0);
}

bool FromJson(const std::string& text, Rules* out, std::string* error) {
    json::Value v;
    std::string err;
    if (text.size() > 4096 || !json::parse(text, v, err) || v.type != json::Value::Type::Object) {
        if (error) *error = err.empty() ? "not a JSON object" : err;
        return false;
    }
    Rules r;
    if (const json::Value* m = v.find("rot"); m && m->type == json::Value::Type::Bool) r.rot = m->boolean;
    const auto num = [&](const char* key, int* to) {
        const json::Value* m = v.find(key);
        if (m && m->type == json::Value::Type::Number && std::isfinite(m->number) && std::fabs(m->number) < 1e6) {
            *to = static_cast<int>(std::lround(m->number));
        }
    };
    num("boss_hp", &r.boss_hp);
    num("boss_damage", &r.boss_damage);
    num("boss_poise", &r.boss_poise);
    num("enemy_hp", &r.enemy_hp);
    num("enemy_damage", &r.enemy_damage);
    num("enemy_poise", &r.enemy_poise);
    *out = Clamped(r);
    return true;
}

namespace {

Doping Make(int hp, int damage, int poise, int cooperators) {
    Doping d;
    const int n = std::clamp(cooperators, 0, 3);
    if (n == 0) return d;
    const float step = kVanillaHpStep * static_cast<float>(hp) / 100.0f;
    d.hp_rate = 1.0f + static_cast<float>(n) * step;
    d.resist_rate = d.hp_rate;
    d.attack_rate = 1.0f + static_cast<float>(n) * static_cast<float>(damage) / 100.0f;
    d.super_armor = static_cast<int>(std::lround(static_cast<double>(n) * kVanillaPoiseStep * poise / 100.0));
    return d;
}

}  // namespace

Doping Boss(const Rules& r, int cooperators) {
    const Rules c = Clamped(r);
    return Make(c.boss_hp, c.boss_damage, c.boss_poise, cooperators);
}

Doping Enemy(const Rules& r, int cooperators) {
    const Rules c = Clamped(r);
    return Make(c.enemy_hp, c.enemy_damage, c.enemy_poise, cooperators);
}

bool EnemyScalingActive(const Rules& r) {
    const Rules c = Clamped(r);
    return c.enemy_hp != 0 || c.enemy_damage != 0 || c.enemy_poise != 0;
}

float RotHpFactor(int stacks) {
    const int s = std::clamp(stacks, 0, kRotMaxStacks);
    return 1.0f - static_cast<float>(s * kRotHpPercentPerStack) / 100.0f;
}

bool LampWarpIsRespawn(bool alive_at_warp, double since_death_s, bool up_since_death) {
    if (!alive_at_warp) return true;
    return since_death_s >= 0 && since_death_s < kRespawnWarpS && !up_since_death;
}

bool LampMenuRest(bool dialog_opened, bool alive, float lamp_dx, float lamp_dy, float lamp_dz) {
    return dialog_opened && alive && lamp_dx * lamp_dx + lamp_dz * lamp_dz <= kLampMenuRadiusM * kLampMenuRadiusM &&
           std::fabs(lamp_dy) <= kLampMenuHeightM;
}

std::string DescribeHp(int percent, bool russian) {
    char b[96];
    const int per = static_cast<int>(std::lround(percent * kVanillaHpStep));
    std::snprintf(b, sizeof b, russian ? "%d %% (+%d %% здоровья за охотника)" : "%d %% (+%d %% HP per extra hunter)",
                  percent, per);
    return b;
}

std::string DescribeDamage(int percent, bool russian) {
    char b[96];
    std::snprintf(b, sizeof b, russian ? "+%d %% урона за охотника" : "+%d %% damage per extra hunter", percent);
    return b;
}

std::string DescribePoise(int percent, bool russian) {
    char b[96];
    const int per = static_cast<int>(std::lround(percent * kVanillaPoiseStep / 100.0));
    std::snprintf(b, sizeof b, russian ? "%d %% (%+d стойкости за охотника)" : "%d %% (%+d poise per extra hunter)",
                  percent, per);
    return b;
}

}  // namespace coop::balance
