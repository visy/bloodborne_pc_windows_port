// SPDX-License-Identifier: GPL-3.0-or-later
#include "build.h"

#include "container.h"
#include "derive_ids.h"
#include "party/party_gamedata.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <thread>

namespace coop::gamedata {
namespace fs = std::filesystem;
namespace {

// Bump when the derivation changes what the tables hold (a cache of another version is rebuilt).
constexpr const char* kDerivationVersion = "cpp-1";
constexpr const char* kCacheMagic = "BBPARTYTABLES 1";

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Files under `dir` (recursive) whose name matches, sorted like the Python tools (case folded path parts).
std::vector<fs::path> ListFiles(const fs::path& dir, const std::function<bool(const std::string&)>& match) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return out;
    for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        std::error_code e2;
        if (it->is_regular_file(e2) && match(it->path().filename().string())) out.push_back(it->path());
    }
    // Python's sort of Windows paths: tuple of case folded parts (not the joined string)
    std::vector<std::pair<std::vector<std::string>, fs::path>> keyed;
    keyed.reserve(out.size());
    for (const fs::path& p : out) {
        std::vector<std::string> parts;
        for (const fs::path& part : p.lexically_relative(dir)) parts.push_back(Lower(part.string()));
        keyed.emplace_back(std::move(parts), p);
    }
    std::sort(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (std::size_t i = 0; i < out.size(); ++i) out[i] = keyed[i].second;
    return out;
}

bool Contains(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }
bool EndsWith(const std::string& s, const char* suffix) {
    const std::size_t n = std::char_traits<char>::length(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// m12_34_56_78 [.msb [.dcx]]
bool MapName(const std::string& name, int* a, int* b, int* c, int* d, bool* has_ext) {
    static const std::regex re(R"(^m(\d\d)_(\d\d)_(\d\d)_(\d\d)(\.msb(\.dcx)?)?$)");
    std::smatch m;
    if (!std::regex_match(name, m, re)) return false;
    *a = std::stoi(m[1]);
    *b = std::stoi(m[2]);
    *c = std::stoi(m[3]);
    *d = std::stoi(m[4]);
    *has_ext = m[5].matched;
    return true;
}

std::int64_t Mtime(const fs::path& p) {
    std::error_code ec;
    const auto t = fs::last_write_time(p, ec);
    return ec ? 0 : static_cast<std::int64_t>(t.time_since_epoch().count());
}

struct Fnv {
    std::uint64_t h1 = 0xcbf29ce484222325ull, h2 = 0x84222325cbf29ce4ull;
    void Add(const std::string& s) {
        for (const char ch : s) {
            const auto c = static_cast<unsigned char>(ch);
            h1 = (h1 ^ c) * 0x100000001b3ull;
            h2 = (h2 ^ (c + 31u)) * 0x1000193ull + 0x9e3779b97f4a7c15ull;
        }
        h1 = (h1 ^ 0xff) * 0x100000001b3ull;
        h2 = (h2 ^ 0xff) * 0x1000193ull;
    }
    std::string Hex() const {
        char buf[40];
        std::snprintf(buf, sizeof buf, "%016llx%016llx", static_cast<unsigned long long>(h1), static_cast<unsigned long long>(h2));
        return buf;
    }
};

struct EventFile {
    std::string stem;
    fs::path path, eld;
    std::uint64_t size = 0;
};

struct Parsed {
    bbdata::Emevd evd;
    std::unordered_map<std::int64_t, std::string> names;
};

bbdata::Emedf LoadEmedf(const fs::path& event_dir) {
    for (const char* n : {"common.emedf.dcx", "common.emedf"}) {
        const fs::path p = event_dir / n;
        std::error_code ec;
        if (fs::exists(p, ec)) return bbdata::ParseEmedf(bbdata::Bytes(bbdata::ReadMaybeDcx(p.string())));
    }
    throw bbdata::FormatError("common.emedf(.dcx) not found in the event folder");
}

bbdata::ParamTable LoadParam(const bbdata::Bytes& pb, const std::vector<bbdata::BndEntry>& pe, const bbdata::Bytes& db,
                             const std::vector<bbdata::BndEntry>& de, const std::string& name) {
    const bbdata::BndEntry* p = bbdata::FindBndEntry(pe, name + ".param");
    const bbdata::BndEntry* d = bbdata::FindBndEntry(de, name + ".paramdef");
    if (!p || !d) throw bbdata::FormatError("param " + name + " not found");
    return bbdata::ParseParam(bbdata::Span(pb).Sub(static_cast<std::int64_t>(p->offset), p->size),
                              bbdata::Span(db).Sub(static_cast<std::int64_t>(d->offset), d->size));
}

}  // namespace

std::string GameRoot(const std::string& path) {
    std::error_code ec;
    const fs::path p = fs::path(path) / "dvdroot_ps4";
    return fs::is_directory(p, ec) ? p.string() : path;
}

std::string ComputeKey(const std::string& game_root) {
    const fs::path root(game_root);
    Fnv h;
    h.Add(std::string("derivation ") + kDerivationVersion);
    const fs::path pb = root / "param/gameparam/gameparam.parambnd.dcx", db = root / "paramdef/paramdef.paramdefbnd.dcx";
    std::error_code ec;
    if (!fs::is_regular_file(pb, ec)) return std::string();
    auto file = [&](const fs::path& p, const fs::path& base) {
        std::error_code e;
        h.Add(fs::relative(p, base).generic_string() + "|" + std::to_string(fs::file_size(p, e)) + "|" + std::to_string(Mtime(p)));
    };
    file(pb, root);
    if (fs::is_regular_file(db, ec)) file(db, root);
    for (const fs::path& p : ListFiles(root / "event", [](const std::string&) { return true; })) file(p, root);
    for (const fs::path& p : ListFiles(root / "map/mapstudio", [](const std::string& n) { return Contains(n, ".msb"); })) file(p, root);
    for (const char* sub : {"map", "remo"}) {
        std::vector<std::string> names;
        if (fs::is_directory(root / sub, ec)) {
            for (const auto& e : fs::directory_iterator(root / sub, ec)) names.push_back(e.path().filename().string());
        }
        std::sort(names.begin(), names.end());
        h.Add(std::string(sub) + ":");
        for (const std::string& n : names) h.Add(n);
    }
    return h.Hex();
}

Sections DeriveAll(const std::string& game_root, unsigned threads, Progress* progress, Timings* timings) {
    const fs::path root(game_root);
    Progress local;
    Progress& prog = progress ? *progress : local;
    std::error_code ec;

    // ---- listing ----
    std::vector<EventFile> files;
    std::vector<MsbFile> msbs;
    std::set<std::int64_t> maps, remos;
    {
        StepTimer t(timings, "list");
        prog.SetLabel("Reading the game's file list");
        for (const fs::path& p : ListFiles(root / "event", [](const std::string& n) { return Contains(n, ".emevd"); })) {
            EventFile f;
            const std::string name = p.filename().string();
            f.stem = name.substr(0, name.find('.'));
            f.path = p;
            f.eld = p.parent_path() / (f.stem + ".emeld" + (EndsWith(name, ".dcx") ? ".dcx" : ""));
            f.size = fs::file_size(p, ec);
            files.push_back(std::move(f));
        }
        if (fs::is_directory(root / "map", ec)) {
            for (const auto& e : fs::directory_iterator(root / "map", ec)) {
                int a, b, c, d;
                bool ext;
                if (e.is_directory(ec) && MapName(e.path().filename().string(), &a, &b, &c, &d, &ext)) {
                    maps.insert(a << 24 | b << 16 | c << 8 | d);
                }
            }
        }
        for (const fs::path& p : ListFiles(root / "map/mapstudio", [](const std::string& n) { return Contains(n, ".msb"); })) {
            int a, b, c, d;
            bool ext;
            if (!MapName(p.filename().string(), &a, &b, &c, &d, &ext)) continue;
            maps.insert(a << 24 | b << 16 | c << 8 | d);
            MsbFile m;
            m.a = a;
            m.b = b;
            m.c = c;
            m.d = d;
            m.path = p.string();
            m.size = fs::file_size(p, ec);
            msbs.push_back(std::move(m));
        }
        static const std::regex remo_re(R"(^s(\d\d)_(\d\d)_(\d{4})\.remobnd(\.dcx)?$)");
        if (fs::is_directory(root / "remo", ec)) {
            for (const auto& e : fs::directory_iterator(root / "remo", ec)) {
                std::smatch m;
                const std::string n = e.path().filename().string();
                if (std::regex_match(n, m, remo_re)) remos.insert(std::stoll(m[1]) * 1000000 + std::stoll(m[2]) * 10000 + std::stoll(m[3]));
            }
        }
    }
    const fs::path pb_path = root / "param/gameparam/gameparam.parambnd.dcx", db_path = root / "paramdef/paramdef.paramdefbnd.dcx";
    const std::uint64_t param_bytes = fs::file_size(pb_path, ec) + fs::file_size(db_path, ec);
    std::uint64_t total = param_bytes + 4096;
    for (const EventFile& f : files) total += 2 * f.size;
    for (const MsbFile& m : msbs) total += m.size;
    prog.SetTotal(total);

    // ---- phase 1: params, event scripts, maps, all in parallel (heaviest first) ----
    prog.SetLabel("Reading params, events and maps");
    ParamSet params;
    std::vector<Parsed> parsed(files.size());
    std::vector<MsbInfo> infos(msbs.size());
    bbdata::Emedf emedf;
    {
        StepTimer t(timings, "read");
        emedf = LoadEmedf(root / "event");
        struct Task {
            std::uint64_t weight;
            std::function<void()> run;
        };
        std::vector<Task> tasks;
        tasks.push_back({param_bytes, [&] {
                             StepTimer pt(timings, "  params");
                             const bbdata::Bytes pb = bbdata::ReadMaybeDcx(pb_path.string()), db = bbdata::ReadMaybeDcx(db_path.string());
                             const auto pe = bbdata::ParseBnd4(pb), de = bbdata::ParseBnd4(db);
                             params.item_lot = LoadParam(pb, pe, db, de, "ItemLotParam");
                             params.goods = LoadParam(pb, pe, db, de, "EquipParamGoods");
                             params.weapon = LoadParam(pb, pe, db, de, "EquipParamWeapon");
                             params.protector = LoadParam(pb, pe, db, de, "EquipParamProtector");
                             params.return_point = LoadParam(pb, pe, db, de, "ReturnPointParam");
                             params.shop = LoadParam(pb, pe, db, de, "ShopLineupParam");
                         }});
        for (std::size_t i = 0; i < files.size(); ++i) {
            tasks.push_back({files[i].size, [&, i] {
                                 parsed[i].evd = bbdata::ParseEmevd(bbdata::ReadMaybeDcx(files[i].path.string()));
                                 std::error_code e;
                                 if (fs::exists(files[i].eld, e)) {
                                     parsed[i].names = bbdata::ParseEmeld(bbdata::Span(bbdata::ReadMaybeDcx(files[i].eld.string())));
                                 }
                             }});
        }
        for (std::size_t i = 0; i < msbs.size(); ++i) {
            tasks.push_back({msbs[i].size, [&, i] { infos[i] = ReadMsb(msbs[i]); }});
        }
        std::vector<std::size_t> order(tasks.size());
        for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return tasks[a].weight > tasks[b].weight; });
        ParallelFor(tasks.size(), threads, [&](std::size_t k) {
            Task& t2 = tasks[order[k]];
            t2.run();
            prog.Add(t2.weight);
        });
    }

    // ---- phase 2: expand the events (map scripts in parallel, then common with their calls into it) ----
    prog.SetLabel("Expanding event scripts");
    std::vector<FileEvents> allrec;
    {
        StepTimer t(timings, "expand");
        std::size_t common = files.size();
        for (std::size_t i = 0; i < files.size(); ++i) {
            if (files[i].stem == "common") {
                common = i;
                break;
            }
        }
        std::set<std::int64_t> common_ids;
        if (common < files.size()) {
            for (const bbdata::EvEvent& e : parsed[common].evd.events) common_ids.insert(e.id);
        }
        std::vector<std::vector<bbdata::EventRec>> recs(files.size());
        const bbdata::ExternalCalls none;
        ParallelFor(files.size(), threads, [&](std::size_t i) {
            if (files[i].stem == "common") return;
            recs[i] = bbdata::ExpandFile(parsed[i].evd, parsed[i].names, emedf, none);
            prog.Add(files[i].size);
        });
        bbdata::ExternalCalls external;
        for (std::size_t i = 0; i < files.size(); ++i) {
            if (files[i].stem == "common") continue;
            std::set<std::int64_t> own;
            for (const bbdata::EventRec& r : recs[i]) own.insert(r.event);
            for (const bbdata::EventRec& r : recs[i]) {
                for (const bbdata::EventCall& c : r.calls) {
                    if (common_ids.count(c.target) && !own.count(c.target)) external[c.target].push_back({c.slot, c.params});
                }
            }
        }
        for (std::size_t i = 0; i < files.size(); ++i) {
            if (files[i].stem != "common") allrec.push_back({files[i].stem, std::move(recs[i])});
        }
        if (common < files.size()) {
            allrec.push_back({"common", bbdata::ExpandFile(parsed[common].evd, parsed[common].names, emedf, external)});
            prog.Add(files[common].size);
        }
    }

    // ---- phase 3: the tables, independent of one another ----
    prog.SetLabel("Deriving the tables");
    std::vector<FogCalls> fog_calls;
    {
        std::map<std::string, FogCalls> by_stem;
        for (std::size_t i = 0; i < files.size(); ++i) {
            for (FogCalls& fc : CollectFogCalls(files[i].stem, parsed[i].evd)) {
                FogCalls& dst = by_stem[fc.stem];
                dst.stem = fc.stem;
                for (auto& c : fc.calls) dst.calls.push_back(std::move(c));
            }
        }
        for (auto& [k, v] : by_stem) fog_calls.push_back(std::move(v));
    }
    parsed.clear();
    parsed.shrink_to_fit();
    Sections out[4];
    {
        StepTimer t(timings, "derive");
        IdsInput in;
        in.maps = maps;
        in.remo_files = remos;
        in.msbs = &msbs;
        in.infos = &infos;
        in.params = &params;
        in.events = &allrec;
        ParallelFor(4, threads, [&](std::size_t k) {
            switch (k) {
                case 0: { StepTimer s(timings, "  items"); out[0] = DeriveItems(params, allrec); break; }
                case 1: { StepTimer s(timings, "  ids"); out[1] = DeriveIds(in); break; }
                case 2: { StepTimer s(timings, "  warper"); out[2] = DeriveWarper(params); break; }
                default: { StepTimer s(timings, "  npcfog"); out[3] = DeriveNpcFog(fog_calls, (root / "map/mapstudio").string(), threads); break; }
            }
        });
    }
    prog.Add(4096);
    Sections all;
    for (Sections& s : out) {
        for (auto& [name, rows] : s) all[name] = std::move(rows);
    }
    return all;
}

std::string RenderCache(const std::string& key, const Sections& sections) {
    std::string t = std::string(kCacheMagic) + "\nkey " + key + "\n";
    for (const auto& [name, rows] : sections) {
        t += "[" + name + "] " + std::to_string(rows.size()) + "\n";
        for (const Row& r : rows) {
            for (std::size_t i = 0; i < r.size(); ++i) {
                if (r[i].find_first_of("\t\n") != std::string::npos) throw std::runtime_error(name + ": tab / newline in a field");
                if (i) t += '\t';
                t += r[i];
            }
            t += '\n';
        }
    }
    return t;
}

std::string CacheTextKey(const std::string& text) {
    std::istringstream in(text);
    std::string l1, l2;
    std::getline(in, l1);
    std::getline(in, l2);
    while (!l1.empty() && (l1.back() == '\r' || l1.back() == ' ')) l1.pop_back();
    while (!l2.empty() && (l2.back() == '\r' || l2.back() == ' ')) l2.pop_back();
    if (l1 != kCacheMagic || l2.rfind("key ", 0) != 0) return std::string();
    return l2.substr(4);
}

namespace {

bool ReadText(const std::string& path, std::string* out) {
    try {
        const bbdata::Bytes b = bbdata::ReadFile(path);
        out->assign(b.begin(), b.end());
        return true;
    } catch (const bbdata::FormatError&) {
        return false;
    }
}

bool WriteAtomic(const std::string& path, const std::string& text) {
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!f) return false;
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(path, ec);
        fs::rename(tmp, path, ec);
    }
    return !ec;
}

std::string TimingLine(Timings& t) {
    std::string s;
    std::lock_guard<std::mutex> lk(t.mu);
    for (const auto& [n, ms] : t.steps) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "%s %.0f ms; ", n.c_str(), ms);
        s += buf;
    }
    return s;
}

/// Cache hit or derive + write. `text` receives the cache text.
bool CachedText(const std::string& root, const std::string& cache_path, bool force, unsigned threads, Progress* prog,
                std::string* text, std::string* err, std::string* log, bool* rebuilt) {
    const std::string key = ComputeKey(root);
    if (key.empty()) {
        *err = "no game data under " + root;
        return false;
    }
    if (!force && ReadText(cache_path, text) && CacheTextKey(*text) == key) {
        *rebuilt = false;
        return true;
    }
    *rebuilt = true;
    Timings tm;
    const auto t0 = std::chrono::steady_clock::now();
    try {
        const Sections s = DeriveAll(root, threads, prog, &tm);
        *text = RenderCache(key, s);
    } catch (const std::exception& e) {
        *err = e.what();
        return false;
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (!WriteAtomic(cache_path, *text) && log) *log += "could not write " + cache_path + "\n";
    char buf[64];
    std::snprintf(buf, sizeof buf, "built in %.1f s with %u threads: ", secs, threads);
    if (log) *log += std::string(buf) + TimingLine(tm) + "\n";
    return true;
}

}  // namespace

bool BuildToFile(const std::string& game, const std::string& cache_path, bool force, unsigned threads, std::string* err,
                 std::string* log) {
    std::string text, e, l;
    bool rebuilt = false;
    Progress prog;
    const bool ok = CachedText(GameRoot(game), cache_path, force, threads ? threads : DefaultThreads(), &prog, &text, &e, &l, &rebuilt);
    if (log) *log = l + (ok && !rebuilt ? "cache current\n" : "");
    if (!ok && err) *err = e;
    return ok;
}

unsigned DefaultThreads() {
    if (const char* e = std::getenv("BB_PARTY_TABLES_THREADS")) {
        const int n = std::atoi(e);
        if (n > 0) return static_cast<unsigned>(n);
    }
    const unsigned hc = std::thread::hardware_concurrency();
    return hc > 2 ? hc - 1 : 1;
}

void StartBackground(const std::string& app0, const std::string& user_dir) {
    const char* tables = std::getenv("BB_PARTY_TABLES");
    const char* off = std::getenv("BB_PARTY_TABLES_BUILD");
    if ((tables && *tables) || (off && std::string(off) == "0")) return;  // a named cache file / switched off: Get() reads the file
    std::string game = app0;
    if (game.empty()) {
        if (const char* g = std::getenv("BB_GAME_DIR")) game = g;
    }
    if (game.empty()) return;
    const std::string cache = (fs::path(user_dir.empty() ? "user" : user_dir) / "party_tables.txt").string();
    BeginBuild();
    std::thread([game, cache] {
        const auto t0 = std::chrono::steady_clock::now();
        Tables tables_out;
        std::string err;
        try {
            const std::string root = GameRoot(game);
            const unsigned threads = DefaultThreads();
            Progress prog;
            std::atomic<bool> done{false};
            std::thread reporter([&] {
                std::string last;
                int last_pct = -1;
                while (!done.load()) {
                    const std::string label = prog.Label();
                    const int pct = prog.Percent();
                    SetBuildProgress(pct, label);
                    if (label != last && !label.empty()) {
                        std::printf("[party-data] %s: %d%%\n", label.c_str(), pct);
                        std::fflush(stdout);
                        last = label;
                        last_pct = pct;
                    }
                    (void)last_pct;
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
            });
            std::string text, log;
            bool rebuilt = false;
            const bool ok = CachedText(root, cache, false, threads, &prog, &text, &err, &log, &rebuilt);
            done.store(true);
            reporter.join();
            if (ok) {
                std::string perr;
                if (Parse(text, &tables_out, &perr)) {
                    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                    if (rebuilt) {
                        std::printf("[party-data] done in %.1f s: %s", secs, log.c_str());
                    } else {
                        std::printf("[party-data] tables loaded from the cache (%.2f s)\n", secs);
                    }
                } else {
                    err = perr;
                }
            }
        } catch (const std::exception& e) {
            err = e.what();
        }
        if (!tables_out.loaded) {
            std::printf("[party-data] could not be built (%s); the party features that need the tables are off\n", err.c_str());
            tables_out = Tables{};
            tables_out.status = "party tables not built (" + err + ")";
        }
        std::fflush(stdout);
        FinishBuild(std::move(tables_out));
    }).detach();
}

}  // namespace coop::gamedata
