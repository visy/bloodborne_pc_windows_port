// SPDX-License-Identifier: GPL-3.0-or-later
// gpu/shim/gamedata: the readers (DCX, BND4, PARAM + PARAMDEF, EMEDF / EMEVD / EMELD, MSB) and the table
// derivation, on SYNTHETIC data built here (no game bytes). The equality with the Python oracle on a real game
// is tests/test_party_tables.py. cmake --build out/gpu --target gamedata-test
#include "gamedata/build.h"
#include "gamedata/container.h"
#include "gamedata/events.h"
#include "gamedata/msb.h"
#include "gamedata/param.h"
#include "party/party_gamedata.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

using namespace bbdata;
namespace fs = std::filesystem;

static int g_fail = 0;
#define CHECK(c)                                                     \
    do {                                                             \
        if (!(c)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++g_fail;                                                \
        }                                                            \
    } while (0)

namespace {

template <class T>
void Put(Bytes& b, std::size_t at, T v) {
    if (b.size() < at + sizeof(T)) b.resize(at + sizeof(T), 0);
    std::memcpy(b.data() + at, &v, sizeof(T));
}
void PutStr16(Bytes& b, std::size_t at, const std::string& s) {
    for (std::size_t i = 0; i < s.size(); ++i) Put<std::uint16_t>(b, at + i * 2, static_cast<unsigned char>(s[i]));
    Put<std::uint16_t>(b, at + s.size() * 2, 0);
}

struct DefField {
    std::string type, name;
    std::uint32_t size;
};

Bytes MakeParamdef(const std::vector<DefField>& fields) {
    Bytes b(0x38 + fields.size() * 0xd0, 0);
    Put<std::uint16_t>(b, 8, static_cast<std::uint16_t>(fields.size()));
    for (std::size_t i = 0; i < fields.size(); ++i) {
        const std::size_t o = 0x38 + i * 0xd0;
        std::memcpy(b.data() + o + 0x70, fields[i].type.data(), fields[i].type.size());
        std::memcpy(b.data() + o + 0x90, fields[i].name.data(), fields[i].name.size());
        Put<std::uint32_t>(b, o + 0x64, fields[i].size);
    }
    return b;
}

// rows: {id, row bytes}
Bytes MakeParam(const std::vector<std::pair<std::int32_t, Bytes>>& rows) {
    Bytes b(0x40 + rows.size() * 0x18, 0);
    Put<std::uint16_t>(b, 0x0a, static_cast<std::uint16_t>(rows.size()));
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const std::size_t at = b.size();
        b.insert(b.end(), rows[i].second.begin(), rows[i].second.end());
        Put<std::int32_t>(b, 0x40 + i * 0x18, rows[i].first);
        Put<std::int64_t>(b, 0x40 + i * 0x18 + 8, static_cast<std::int64_t>(at));
    }
    return b;
}

Bytes MakeBnd4(const std::vector<std::pair<std::string, Bytes>>& entries) {
    const std::size_t esz = 0x24;
    Bytes b(0x40 + entries.size() * esz, 0);
    std::memcpy(b.data(), "BND4", 4);
    Put<std::uint32_t>(b, 0x0c, static_cast<std::uint32_t>(entries.size()));
    Put<std::uint64_t>(b, 0x20, esz);
    Put<std::uint8_t>(b, 0x30, 1);
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const std::size_t name_at = b.size();
        b.resize(name_at + (entries[i].first.size() + 1) * 2, 0);
        PutStr16(b, name_at, entries[i].first);
        const std::size_t data_at = b.size();
        b.insert(b.end(), entries[i].second.begin(), entries[i].second.end());
        const std::size_t o = 0x40 + i * esz;
        Put<std::int64_t>(b, o + 8, static_cast<std::int64_t>(entries[i].second.size()));
        Put<std::uint32_t>(b, o + 0x18, static_cast<std::uint32_t>(data_at));
        Put<std::uint32_t>(b, o + 0x20, static_cast<std::uint32_t>(name_at));
    }
    return b;
}

struct SynInstr {
    std::uint32_t bank, id;
    Bytes args;
};
struct SynParam {
    std::int64_t instr, target, source;
    std::int32_t count;
};
struct SynEvent {
    std::int64_t id;
    std::uint32_t rest;
    std::vector<SynInstr> ins;
    std::vector<SynParam> params;
};

Bytes MakeEmevd(const std::vector<SynEvent>& events) {
    Bytes b(0x100, 0);
    std::memcpy(b.data(), "EVD\0", 4);
    Put<std::uint32_t>(b, 8, 0xCC);
    std::size_t n_ins = 0, n_par = 0;
    for (const SynEvent& e : events) n_ins += e.ins.size(), n_par += e.params.size();
    const std::size_t ev_o = b.size();
    b.resize(ev_o + events.size() * 0x30);
    const std::size_t in_o = b.size();
    b.resize(in_o + n_ins * 0x20);
    const std::size_t pa_o = b.size();
    b.resize(pa_o + n_par * 0x20);
    const std::size_t args_o = b.size();
    std::size_t ii = 0, pi = 0, args = 0;
    for (std::size_t k = 0; k < events.size(); ++k) {
        const SynEvent& e = events[k];
        const std::size_t eo = ev_o + k * 0x30;
        Put<std::int64_t>(b, eo, e.id);
        Put<std::int64_t>(b, eo + 8, static_cast<std::int64_t>(e.ins.size()));
        Put<std::int64_t>(b, eo + 16, static_cast<std::int64_t>(ii * 0x20));
        Put<std::int64_t>(b, eo + 24, static_cast<std::int64_t>(e.params.size()));
        Put<std::int64_t>(b, eo + 32, static_cast<std::int64_t>(pi * 0x20));
        Put<std::uint32_t>(b, eo + 40, e.rest);
        for (const SynInstr& in : e.ins) {
            const std::size_t io = in_o + ii * 0x20;
            Put<std::uint32_t>(b, io, in.bank);
            Put<std::uint32_t>(b, io + 4, in.id);
            Put<std::int64_t>(b, io + 8, static_cast<std::int64_t>(in.args.size()));
            Put<std::int64_t>(b, io + 16, static_cast<std::int64_t>(args));
            b.resize(args_o + args + in.args.size());
            std::memcpy(b.data() + args_o + args, in.args.data(), in.args.size());
            args += in.args.size();
            ++ii;
        }
        for (const SynParam& p : e.params) {
            const std::size_t po = pa_o + pi * 0x20;
            Put<std::int64_t>(b, po, p.instr);
            Put<std::int64_t>(b, po + 8, p.target);
            Put<std::int64_t>(b, po + 16, p.source);
            Put<std::int32_t>(b, po + 24, p.count);
            ++pi;
        }
    }
    Put<std::int64_t>(b, 0x10, static_cast<std::int64_t>(events.size()));
    Put<std::int64_t>(b, 0x18, static_cast<std::int64_t>(ev_o));
    Put<std::int64_t>(b, 0x28, static_cast<std::int64_t>(in_o));
    Put<std::int64_t>(b, 0x10 + 8 * 8, static_cast<std::int64_t>(n_par));
    Put<std::int64_t>(b, 0x10 + 9 * 8, static_cast<std::int64_t>(pa_o));
    Put<std::int64_t>(b, 0x10 + 13 * 8, static_cast<std::int64_t>(args_o));
    return b;
}

struct SynDef {
    std::int64_t bank, id;
    std::vector<std::pair<std::string, std::uint32_t>> args;
};

Bytes MakeEmedf(const std::vector<SynDef>& defs) {
    // header 0xb0, one class, instructions, args, strings
    Bytes b(0xb0, 0);
    std::memcpy(b.data(), "EDF\0", 4);
    const std::size_t ocls = b.size();
    b.resize(ocls + 32);
    const std::size_t oins = b.size();
    b.resize(oins + defs.size() * 32);
    std::size_t n_args = 0;
    for (const SynDef& d : defs) n_args += d.args.size();
    const std::size_t oarg = b.size();
    b.resize(oarg + n_args * 64);
    const std::size_t ostr = b.size();
    std::size_t str_at = 2;  // offset 0 is the empty name
    b.resize(ostr + 2);
    std::size_t ai = 0;
    for (std::size_t i = 0; i < defs.size(); ++i) {
        const std::size_t io = oins + i * 32;
        Put<std::int64_t>(b, io, defs[i].id);
        Put<std::int64_t>(b, io + 8, static_cast<std::int64_t>(defs[i].args.size()));
        Put<std::int64_t>(b, io + 16, static_cast<std::int64_t>(ai * 64));
        for (const auto& a : defs[i].args) {
            const std::size_t ao = oarg + ai * 64;
            Put<std::int64_t>(b, ao, static_cast<std::int64_t>(str_at));
            Put<std::int64_t>(b, ao + 8, a.second);
            b.resize(ostr + str_at + (a.first.size() + 1) * 2);
            PutStr16(b, ostr + str_at, a.first);
            str_at += (a.first.size() + 1) * 2;
            ++ai;
        }
    }
    // all instructions under one class (bank of the first; the party tooling keys by the class's bank)
    Put<std::int64_t>(b, ocls, defs.empty() ? 0 : defs[0].bank);
    Put<std::int64_t>(b, ocls + 8, static_cast<std::int64_t>(defs.size()));
    Put<std::int64_t>(b, ocls + 16, 0);
    Put<std::int64_t>(b, 0x10, 1);
    Put<std::int64_t>(b, 0x18, static_cast<std::int64_t>(ocls));
    Put<std::int64_t>(b, 0x20, static_cast<std::int64_t>(defs.size()));
    Put<std::int64_t>(b, 0x28, static_cast<std::int64_t>(oins));
    Put<std::int64_t>(b, 0x30, static_cast<std::int64_t>(n_args));
    Put<std::int64_t>(b, 0x38, static_cast<std::int64_t>(oarg));
    Put<std::int64_t>(b, 0x10 + 8 * 16 + 8, static_cast<std::int64_t>(ostr));
    return b;
}

void TestContainers() {
    Bytes raw(5000);
    for (std::size_t i = 0; i < raw.size(); ++i) raw[i] = static_cast<std::uint8_t>(i % 7);
    const Bytes dcx = MakeDcx(raw);
    CHECK(dcx.size() < raw.size() && std::memcmp(dcx.data(), "DCX\0", 4) == 0);
    CHECK(DecodeMaybeDcx(dcx) == raw);
    CHECK(DecodeMaybeDcx(raw) == raw);  // not a DCX: unchanged
    Bytes other = dcx;
    for (int i = 0; i < 4; ++i) other.at(0x28 + static_cast<std::size_t>(i)) = static_cast<std::uint8_t>("KRAK"[i]);
    bool threw = false;
    try {
        DecodeMaybeDcx(other);
    } catch (const FormatError&) {
        threw = true;
    }
    CHECK(threw);
    Bytes cut(dcx.begin(), dcx.begin() + 0x60);
    threw = false;
    try {
        DecodeMaybeDcx(cut);
    } catch (const FormatError&) {
        threw = true;
    }
    CHECK(threw);

    const Bytes bnd = MakeBnd4({{"N:\\x\\param\\gameparam\\AParam.param", {1, 2, 3}}, {"N:\\x\\paramdef\\BParam.paramdef", {9}}});
    const auto es = ParseBnd4(bnd);
    CHECK(es.size() == 2);
    const BndEntry* a = FindBndEntry(es, "AParam.param");
    CHECK(a && a->size == 3 && bnd[a->offset] == 1);
    CHECK(!FindBndEntry(es, "Nope.param"));
    threw = false;
    try {
        Bytes bad = bnd;
        Put<std::int64_t>(bad, 0x40 + 8, 1 << 20);
        ParseBnd4(bad);
    } catch (const FormatError&) {
        threw = true;
    }
    CHECK(threw);
}

void TestParam() {
    const Bytes def = MakeParamdef({{"s32", "flag", 4}, {"u8", "a:3", 1}, {"u8", "b:5", 1}, {"dummy8", "pad[1]", 1}, {"s16", "neg", 2}, {"u16", "big", 2}});
    Bytes r1(10, 0), r2(10, 0);
    Put<std::int32_t>(r1, 0, -5);
    Put<std::uint8_t>(r1, 4, static_cast<std::uint8_t>(5 | 17 << 3));  // a = 5, b = 17
    Put<std::int16_t>(r1, 6, -2);
    Put<std::uint16_t>(r1, 8, 65535);
    Put<std::int32_t>(r2, 0, 77);
    const Bytes prm = MakeParam({{100, r1}, {200, r2}});
    const ParamTable t = ParseParam(prm, def);
    CHECK(t.Rows() == 2 && t.ids[1] == 200);
    CHECK(t.Get(0, "flag") == -5 && t.Get(0, "a") == 5 && t.Get(0, "b") == 17);
    CHECK(t.Get(0, "neg") == -2 && t.Get(0, "big") == 65535 && t.Get(1, "flag") == 77);
    CHECK(t.Col("pad") < 0 && t.Get(0, "missing", 42) == 42);
}

void TestEvents() {
    const Bytes edf = MakeEmedf({{2003, 2, {{"flag", 2}, {"state", 0}}}, {2003, 4, {{"lot", 5}}}});
    const Emedf emedf = ParseEmedf(edf);
    CHECK(emedf.Find(2003, 2) && emedf.Find(2003, 2)->args.size() == 2 && !emedf.Find(2003, 3));
    Bytes set_args(5, 0);
    Put<std::uint32_t>(set_args, 0, 111);
    Put<std::uint8_t>(set_args, 4, 1);
    Bytes call(12, 0);
    Put<std::int32_t>(call, 0, 3);    // slot
    Put<std::uint32_t>(call, 4, 200);  // target event
    Put<std::uint32_t>(call, 8, 12345678);  // its parameter
    Bytes lot(4, 0);
    Put<std::int32_t>(lot, 0, 5000);
    const Bytes evd = MakeEmevd({{0, 0, {{2000, 0, call}}, {}},
                                 {200, 1, {{2003, 2, set_args}, {2003, 4, lot}}, {{0, 0, 0, 4}}}});
    const Emevd e = ParseEmevd(evd);
    CHECK(e.events.size() == 2 && e.events[1].params.size() == 1);
    const auto recs = ExpandFile(e, {{200, "name"}}, emedf, {});
    CHECK(recs.size() == 2);
    const EventRec* r = nullptr;
    for (const EventRec& x : recs) {
        if (x.event == 200) r = &x;
    }
    CHECK(r && r->slot == 3 && r->rest == 1 && r->name == "name");
    CHECK(r && r->ops.size() == 1 && r->ops[0].op == FlagOp::Set && r->ops[0].first == 12345678 && r->ops[0].StateIs(1));
    CHECK(r && r->markers.size() == 1 && r->markers[0].kind == MarkerKind::ItemLot && r->markers[0].v[0] == 5000);
    // the same event called from another file (external call) gets its own instance
    ExternalCalls ext;
    Bytes p2(4, 0);
    Put<std::uint32_t>(p2, 0, 777);
    ext[200].push_back({9, std::string(reinterpret_cast<const char*>(p2.data()), 4)});
    const auto recs2 = ExpandFile(e, {}, emedf, ext);
    int n200 = 0;
    for (const EventRec& x : recs2) n200 += x.event == 200;
    CHECK(n200 == 2);
    bool threw = false;
    try {
        Bytes bad = evd;
        bad[8] = 0;
        ParseEmevd(bad);
    } catch (const FormatError&) {
        threw = true;
    }
    CHECK(threw);
}

void TestMsb() {
    // header 0x10, one list "POINT_PARAM_ST" with one entry that has an entity id
    Bytes b(0x200, 0);
    std::memcpy(b.data(), "MSB ", 4);
    const std::size_t list = 0x10, name_at = 0x80, entry = 0x100, ent = 0x1c0;
    Put<std::int32_t>(b, list + 4, 2);  // entries + the next-list offset
    Put<std::int64_t>(b, list + 8, name_at);
    Put<std::int64_t>(b, list + 16, entry);
    Put<std::int64_t>(b, list + 24, 0);
    PutStr16(b, name_at, "POINT_PARAM_ST");
    Put<float>(b, entry + 0x14, 1.5f);
    Put<float>(b, entry + 0x18, -2.f);
    Put<float>(b, entry + 0x1c, 3.f);
    Put<float>(b, entry + 0x24, 90.f);
    Put<std::int64_t>(b, entry + 0x48, ent - entry);
    Put<std::int32_t>(b, ent, 2412951);
    const auto lists = ParseMsbLists(b);
    CHECK(lists.size() == 1 && lists[0].name == "POINT_PARAM_ST" && lists[0].entries.size() == 1);
    const auto pts = ReadMsbPoints(b, *FindMsbList(lists, "POINT_PARAM_ST"), false);
    CHECK(pts.size() == 1 && pts[0].x == 1.5f && pts[0].yaw_deg == 90.f && pts[0].has_entity && pts[0].entity == 2412951);
    std::vector<std::int32_t> ents;
    ScanMsbEntities(b, 2410000, &ents);
    CHECK(ents.size() == 1 && ents[0] == 2412951);
    bool threw = false;
    try {
        b[0] = 'X';
        ScanMsbEntities(b, 0, &ents);
    } catch (const FormatError&) {
        threw = true;
    }
    CHECK(threw);
}

// ---- the derivation on a synthetic game folder, and a "mod" that edits a param ----

void WriteFile(const fs::path& p, const Bytes& b) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
}

struct ParamSpec {
    std::string name;
    std::vector<DefField> fields;
    std::vector<std::pair<std::int32_t, std::vector<std::int64_t>>> rows;  // per field value (all stored as s32 / s16 / u8 by type)
};

Bytes RowBytes(const std::vector<DefField>& fields, const std::vector<std::int64_t>& vals) {
    Bytes b;
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (fields[i].type == "s32") Put<std::int32_t>(b, b.size(), static_cast<std::int32_t>(vals[i]));
        else if (fields[i].type == "s16") Put<std::int16_t>(b, b.size(), static_cast<std::int16_t>(vals[i]));
        else Put<std::uint8_t>(b, b.size(), static_cast<std::uint8_t>(vals[i]));
    }
    return b;
}

void WriteGame(const fs::path& root, const std::vector<ParamSpec>& params) {
    std::vector<std::pair<std::string, Bytes>> pe, de;
    for (const ParamSpec& p : params) {
        std::vector<std::pair<std::int32_t, Bytes>> rows;
        for (const auto& r : p.rows) rows.emplace_back(r.first, RowBytes(p.fields, r.second));
        pe.emplace_back("N:\\p\\" + p.name + ".param", MakeParam(rows));
        de.emplace_back("N:\\p\\" + p.name + ".paramdef", MakeParamdef(p.fields));
    }
    WriteFile(root / "param/gameparam/gameparam.parambnd.dcx", MakeDcx(MakeBnd4(pe)));
    WriteFile(root / "paramdef/paramdef.paramdefbnd.dcx", MakeDcx(MakeBnd4(de)));
    WriteFile(root / "event/common.emedf.dcx", MakeDcx(MakeEmedf({{2003, 2, {{"flag", 2}, {"state", 0}}}})));
}

std::vector<ParamSpec> BaseParams(bool extra_lot) {
    std::vector<ParamSpec> v;
    ParamSpec lot{"ItemLotParam", {{"s32", "getItemFlagId", 4}, {"s32", "lotItemId01", 4}, {"s32", "lotItemCategory01", 4}}, {}};
    lot.rows = {{2000, {50002000, 100, 4}}};
    if (extra_lot) lot.rows.push_back({3000, {50003000, 200, 4}});
    v.push_back(lot);
    v.push_back({"EquipParamGoods", {{"s32", "goodsType", 4}, {"s32", "maxNum", 4}}, {{100, {1, 1}}, {200, {0, 5}}}});
    v.push_back({"EquipParamWeapon", {{"s32", "sortId", 4}, {"s16", "equipModelId", 2}, {"u8", "rightHandEquipable", 1}, {"u8", "leftHandEquipable", 1}},
                 {{1000000, {1, 7, 1, 0}}, {1010000, {2, 7, 1, 0}}, {1020000, {3, 8, 0, 1}}}});
    v.push_back({"EquipParamProtector", {{"s32", "sortId", 4}, {"s16", "equipModelId", 2}, {"u8", "headEquip", 1}}, {{10000, {1, 3, 1}}, {11000, {1, 3, 0}}}});
    v.push_back({"ReturnPointParam", {{"u8", "areaNo", 1}, {"u8", "blockNo", 1}, {"s32", "returnPointEntityId", 4}}, {}});
    v.push_back({"ShopLineupParam", {{"s32", "eventFlag", 4}, {"s32", "qwcId", 4}}, {}});
    return v;
}

void TestDerive() {
    const fs::path d = fs::temp_directory_path() / "bb_gamedata_test";
    std::error_code ec;
    fs::remove_all(d, ec);
    const fs::path root = d / "dvdroot_ps4";
    WriteGame(root, BaseParams(false));
    coop::gamedata::Timings tm;
    coop::gamedata::Progress prog;
    const auto s1 = coop::gamedata::DeriveAll(root.string(), 3, &prog, &tm);
    CHECK(s1.at("itemLotFlag").size() == 1 && s1.at("itemLotFlag")[0][0] == "2000" && s1.at("itemLotFlag")[0][1] == "50002000");
    CHECK(s1.at("warperRight").size() == 1 && s1.at("warperRight")[0][0] == "1000000");  // one per model (7), the 8 is left only
    CHECK(s1.at("warperLeft").size() == 1 && s1.at("warperLeft")[0][0] == "1020000");
    CHECK(s1.at("warperArmor").size() == 1 && s1.at("warperArmor")[0][0] == "10000");
    CHECK(s1.at("maps").empty() && s1.at("npcFog").empty() && s1.at("itemKillAward").size() == 1);
    CHECK(prog.Percent() >= 99);
    // a mod that edits the params: the next build follows it (and its key changes)
    const std::string key1 = coop::gamedata::ComputeKey(root.string());
    CHECK(!key1.empty());
    WriteGame(root, BaseParams(true));
    const auto s2 = coop::gamedata::DeriveAll(root.string(), 1, nullptr, nullptr);
    CHECK(s2.at("itemLotFlag").size() == 2 && s2.at("itemLotFlag")[1][0] == "3000");
    CHECK(coop::gamedata::ComputeKey(root.string()) != key1);
    // cache text round trip through the party parser
    const std::string text = coop::gamedata::RenderCache("k", s2);
    CHECK(coop::gamedata::CacheTextKey(text) == "k");
    coop::gamedata::Tables t;
    std::string err;
    CHECK(coop::gamedata::Parse(text, &t, &err) && t.lot_flags.size() == 2 && t.warper_left.size() == 1);
    // no game data: the key is empty, the build reports it
    CHECK(coop::gamedata::ComputeKey((d / "nothing").string()).empty());
    std::string log;
    CHECK(!coop::gamedata::BuildToFile((d / "nothing").string(), (d / "out.txt").string(), false, 1, &err, &log));
    // BuildToFile: build, then a current cache is left alone
    CHECK(coop::gamedata::BuildToFile(d.string(), (d / "cache.txt").string(), false, 2, &err, &log) && fs::exists(d / "cache.txt"));
    const auto t1 = fs::last_write_time(d / "cache.txt");
    CHECK(coop::gamedata::BuildToFile(d.string(), (d / "cache.txt").string(), false, 2, &err, &log) && log.find("cache current") != std::string::npos);
    CHECK(fs::last_write_time(d / "cache.txt") == t1);
    fs::remove_all(d, ec);
}

void TestParallel() {
    std::atomic<int> sum{0};
    coop::gamedata::ParallelFor(100, 4, [&](std::size_t i) { sum += static_cast<int>(i); });
    CHECK(sum == 4950);
    bool threw = false;
    try {
        coop::gamedata::ParallelFor(10, 4, [&](std::size_t i) {
            if (i == 3 || i == 7) throw std::runtime_error("e" + std::to_string(i));
        });
    } catch (const std::runtime_error& e) {
        threw = std::string(e.what()) == "e3";  // the lowest failing index
    }
    CHECK(threw);
}

}  // namespace

int main() {
    TestContainers();
    TestParam();
    TestEvents();
    TestMsb();
    TestParallel();
    TestDerive();
    std::printf(g_fail ? "gamedata-test: %d FAILED\n" : "gamedata-test: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
