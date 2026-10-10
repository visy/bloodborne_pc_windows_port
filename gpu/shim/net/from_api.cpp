// SPDX-License-Identifier: GPL-3.0-or-later
// FromApi (from_api.h): the FROM game server's API answered by the party host. Formats are in
// from_api_formats.inc.
#include "from_api.h"

#include "bbnet_internal.h"
#include "party_util.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <ctime>
#include <map>

namespace bbnet::party {

#include "from_api_formats.inc"

namespace {

struct Url {
    std::string scheme, host, path, query;
    int port = 0;
};

Url parse_url(const std::string& url) {
    Url u;
    std::size_t p = url.find("://");
    std::size_t h = 0;
    if (p != std::string::npos) {
        u.scheme = url.substr(0, p);
        h = p + 3;
    }
    std::size_t end = url.find_first_of("/?", h);
    std::string hostport = url.substr(h, end == std::string::npos ? std::string::npos : end - h);
    const std::size_t colon = hostport.rfind(':');
    if (colon != std::string::npos) {
        u.port = std::atoi(hostport.c_str() + colon + 1);
        hostport = hostport.substr(0, colon);
    }
    for (char& c : hostport) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    u.host = hostport;
    if (end == std::string::npos) {
        u.path = "/";
        return u;
    }
    const std::size_t q = url.find('?', end);
    u.path = url.substr(end, q == std::string::npos ? std::string::npos : q - end);
    if (u.path.empty()) u.path = "/";
    if (q != std::string::npos) u.query = url.substr(q + 1);
    return u;
}

bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool is_playlog_host(const std::string& host) {
    return host == "bb-playlog-test.s3.amazonaws.com" || host == "bb-playlog-prod.s3.amazonaws.com";
}

// ${Name} -> vars[Name] (raw JSON text), "null" when unknown.
std::string render(const char* tmpl, const std::map<std::string, std::string>& vars) {
    std::string out;
    for (const char* p = tmpl; *p;) {
        if (p[0] == '$' && p[1] == '{') {
            const char* e = std::strchr(p + 2, '}');
            if (e) {
                const std::string key(p + 2, static_cast<std::size_t>(e - p - 2));
                auto it = vars.find(key);
                out += it != vars.end() ? it->second : "null";
                p = e + 1;
                continue;
            }
        }
        out += *p++;
    }
    return out;
}

std::string jstr(const std::string& s) { return json::dump(json::Value(s), 0); }
std::string jnum(long long v) { return std::to_string(v); }

std::string preview(const std::string& body) {
    bool text = true;
    for (unsigned char c : body) {
        if (c < 0x20 && c != '\n' && c != '\r' && c != '\t') {
            text = false;
            break;
        }
    }
    if (text) return body.size() > 600 ? body.substr(0, 600) + "..." : body;
    std::string hex;
    char b[4];
    for (std::size_t i = 0; i < body.size() && i < 96; ++i) {
        std::snprintf(b, sizeof(b), "%02x", static_cast<unsigned char>(body[i]));
        hex += b;
    }
    return "<" + std::to_string(body.size()) + " bytes: " + hex + (body.size() > 96 ? "...>" : ">");
}

// N for the ss.info element names: FrpgNetMan+0x9e8, when the game has it.
bool ss_info_index(unsigned* out) {
    void* slot = guest_image_at(formats::kFrpgNetManSlot, 8);
    if (!slot) return false;
    std::uint64_t man = 0;
    if (!guest_read(reinterpret_cast<std::uintptr_t>(slot), &man, 8) || man < 0x10000) return false;
    std::uint32_t n = 0;
    if (!guest_read(static_cast<std::uintptr_t>(man) + formats::kFrpgNetManServerIndexOff, &n, 4)) return false;
    *out = n;
    return true;
}

}  // namespace

std::string from_api_ss_info() {
    std::vector<unsigned> indices;
    unsigned n = 0;
    if (ss_info_index(&n)) {
        indices.push_back(n);
    } else {
        indices.assign(std::begin(formats::kSsInfoFallbackIndices), std::end(formats::kSsInfoFallbackIndices));
    }
    std::string x;
    x += "<ss>";
    x += formats::kSsStatus;
    x += "</ss>\n";
    for (unsigned i : indices) {
        const std::string sfx = std::to_string(i);
        for (const formats::SsValue& v : formats::kSsValues) {
            x += "<" + std::string(v.name) + sfx + ">" + v.value + "</" + v.name + sfx + ">\n";
        }
        x += "<gameurl" + sfx + ">\n";
        for (const char* api : formats::kApiNames) {
            x += std::string("<") + api + ">" + formats::kApiBase + "</" + api + ">\n";
        }
        x += "</gameurl" + sfx + ">\n";
    }
    return x;
}

FromApi::FromApi(PartyHostService& service) : service_(service) {}

FromApi& FromApi::instance() {
    static FromApi* api = new FromApi(PartyHostService::instance());
    return *api;
}

bool FromApi::routes(const std::string& url) {
    const Url u = parse_url(url);
    if (u.host == kGameHost) return true;
    if (is_playlog_host(u.host)) return true;
    return ends_with(u.host, "scej-network.jp");
}

std::uint64_t FromApi::user_id_of(const std::string& online_id) {
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& [id, uid] : users_) {
        if (id == online_id) return uid;
    }
    users_.emplace_back(online_id, next_user_);
    return next_user_++;
}

std::vector<FromApi::Sign> FromApi::signs() const {
    std::lock_guard<std::mutex> lk(mu_);
    return signs_;
}

void FromApi::handle(const Caller& caller, const HttpRequest& rq, HttpResponse& out) {
    const Url u = parse_url(rq.url);
    out = HttpResponse{};
    if (is_playlog_host(u.host)) {
        out.status = formats::kPlaylogReply.status;
        out.content_type = formats::kPlaylogReply.content_type;
        out.body = formats::kPlaylogReply.body;
    } else if (ends_with(u.path, "ss.info")) {
        out.status = 200;
        out.content_type = "text/plain";
        // The game base64-decodes the ss.info body before parsing it (the completion 0x1e7f240
        // streams [response +0x58] through the base64 decoder 0x1ea92e0 -> 0xfd06f0 at 0x1e8169e,
        // then calls 0x1e89850 -> parser 0x1eb65e0); plain XML decodes to garbage and the parse
        // fails (seen with BB_PARTY_TRACE_SSINFO=1), so no login ever follows.
        out.body = b64_encode(from_api_ss_info());
    } else {
        json::Value body;
        std::string err;
        const bool is_json = !rq.body.empty() && rq.body.find('{') != std::string::npos &&
                             json::parse(rq.body.substr(rq.body.find('{')), body, err);
        if (!is_json) body = json::Value::make_object();
        // ?user_id= on summon_messenger calls (simclient.py): the body's UserId wins.
        if (!body.find("UserId") && u.query.rfind("user_id=", 0) == 0) {
            body.set("UserId", static_cast<long long>(std::strtoll(u.query.c_str() + 8, nullptr, 10)));
        }
        out.body = dispatch(caller, u.path, body, &out.status, &out.content_type);
    }
    if (party_trace()) {
        log("from api: %s %s %s (%s) -> %d %s", caller.online_id.c_str(), rq.method.c_str(), rq.url.c_str(),
            preview(rq.body).c_str(), out.status, preview(out.body).c_str());
    }
}

void FromApi::handle_json(const Caller& caller, const json::Value& rq, json::Value& reply) {
    HttpRequest h;
    h.method = str_of(rq, "Method");
    if (h.method.empty() || h.method.size() > 16) h.method = "GET";
    h.url = str_of(rq, "Url");
    if (h.url.size() > kMaxUrl) {  // a peer's request: nothing the game sends is this long
        reply = json::Value::make_object();
        reply.set("ResKind", 1);
        reply.set("Error", "url too long");
        return;
    }
    if (const json::Value* hs = rq.find("Headers"); hs && hs->type == json::Value::Type::Array) {
        for (const json::Value& v : hs->array) {
            if (h.headers.size() >= kMaxHeaders) break;
            if (v.type == json::Value::Type::String && v.string.size() <= 1024) h.headers.push_back(v.string);
        }
    }
    const std::vector<std::uint8_t> b = b64_decode(str_of(rq, "Body"));
    h.body.assign(b.begin(), b.end());
    HttpResponse r;
    handle(caller, h, r);
    reply = json::Value::make_object();
    reply.set("ResKind", 0);
    reply.set("Status", r.status);
    reply.set("ContentType", r.content_type);
    reply.set("Body", b64_encode(r.body));
}

std::string FromApi::dispatch(const Caller& caller, const std::string& path, const json::Value& body, int* status,
                              std::string* content_type) {
    const formats::Reply* reply = &formats::kDefaultReply;
    for (const formats::Reply& r : formats::kReplies) {
        if (ends_with(path, r.path)) {
            reply = &r;
            break;
        }
    }
    *status = reply->status;
    *content_type = reply->content_type;
    const std::uint64_t uid = user_id_of(caller.online_id);
    std::map<std::string, std::string> vars;
    char sid[32];
    std::snprintf(sid, sizeof(sid), "bbp-%llu", static_cast<unsigned long long>(uid));
    vars["SessionId"] = jstr(sid);
    vars["UserId"] = jnum(static_cast<long long>(uid));
    const std::time_t now = std::time(nullptr);
    vars["Now"] = jnum(static_cast<long long>(now));
    char text[32];
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &now);
#else
    gmtime_r(&now, &tm);
#endif
    std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%S", &tm);  // the game's CheckTime format
    vars["NowText"] = jstr(text);
    // sync_chara_id publishes the CharaId every later request carries; one per member.
    vars["CharaId"] = jnum(int_of(body, "CharaId", static_cast<long long>(uid)));
    if (ends_with(path, "/summon_messenger/create")) vars["SummonDataId"] = sign_create(caller, body);
    else if (ends_with(path, "/summon_messenger/get")) vars["SummonDataList"] = sign_get(caller, body);
    else if (ends_with(path, "/summon_messenger/delete")) sign_delete(caller, body);
    else if (ends_with(path, "/summon_messenger/request")) vars["Result"] = sign_request(caller, body);
    return render(reply->body, vars);
}

std::string FromApi::sign_create(const Caller& caller, const json::Value& body) {
    // The sign goes into other members' games as it is: their native code reads SummonData
    // (exactly 0xE0 bytes, from_api_formats.inc) and the numbers. Anything else is refused.
    std::vector<std::uint8_t> data;
    const long long type = int_of(body, "SummonType", 0);
    const long long version = int_of(body, "SummonDataVersion", 3);
    const long long area = int_of(body, "AreaId", 0);
    const long long region = int_of(body, "AreaRegionId", 0);
    if (!b64_decode_strict(str_of(body, "SummonData"), &data) || data.size() != kSummonDataSize || type < 0 ||
        type > 255 || version < 0 || version > 0xffff || area < 0 || area > 0xffffffffLL || region < 0 ||
        region > 0xffff) {
        log("from api: %s: refused a malformed sign (type %lld, version %lld, %zu data bytes)",
            caller.online_id.c_str(), type, version, data.size());
        return "0";
    }
    Sign s;
    s.user_id = user_id_of(caller.online_id);
    s.online_id = caller.online_id;
    s.area = static_cast<std::uint32_t>(int_of(body, "AreaId", 0));
    s.region = static_cast<int>(int_of(body, "AreaRegionId", 0));
    s.summon_type = static_cast<int>(int_of(body, "SummonType", 0));
    s.data_b64 = str_of(body, "SummonData");
    s.chara_id = int_of(body, "CharaId", static_cast<long long>(s.user_id));
    s.version = version;
    s.request = body;
    std::lock_guard<std::mutex> lk(mu_);
    // One sign per member and type: a new one replaces the old.
    signs_.erase(std::remove_if(signs_.begin(), signs_.end(),
                                [&](const Sign& o) { return o.user_id == s.user_id && o.summon_type == s.summon_type; }),
                 signs_.end());
    std::size_t mine = 0;
    for (const Sign& o : signs_) mine += o.user_id == s.user_id ? 1 : 0;
    if (mine >= kMaxSignsPerUser) {
        log("from api: %s has %zu signs up; refused another", caller.online_id.c_str(), mine);
        return "0";
    }
    s.id = next_sign_++;
    signs_.push_back(s);
    log("from api: %s put up a sign (type %d, area 0x%x) -> %llu", caller.online_id.c_str(), s.summon_type, s.area,
        static_cast<unsigned long long>(s.id));
    return jnum(static_cast<long long>(s.id));
}

std::string FromApi::sign_get(const Caller& caller, const json::Value& body) {
    std::vector<int> types;
    if (const json::Value* tl = body.find("SummonTypeList"); tl && tl->type == json::Value::Type::Array) {
        for (const json::Value& t : tl->array) types.push_back(static_cast<int>(int_of(t, "SummonType", 0)));
    }
    long long max = int_of(body, "GetMaxCount", 32);
    if (max <= 0) max = 32;
    std::string list = "[";
    int n = 0;
    std::lock_guard<std::mutex> lk(mu_);
    // Party-only by construction; every other member's signs, wherever they stand (the party
    // plays together - the game's own area check still applies on its side).
    for (const Sign& s : signs_) {
        if (s.online_id == caller.online_id) continue;
        if (!types.empty() && std::find(types.begin(), types.end(), s.summon_type) == types.end()) continue;
        if (n >= max) break;
        std::map<std::string, std::string> v;
        v["SummonDataId"] = jnum(static_cast<long long>(s.id));
        v["UserId"] = jnum(static_cast<long long>(s.user_id));
        v["SummonType"] = jnum(s.summon_type);
        v["AreaId"] = jnum(s.area);
        v["AreaRegionId"] = jnum(s.region);
        v["SummonData"] = jstr(s.data_b64);
        v["OnlineId"] = jstr(s.online_id);
        v["CharaId"] = jnum(s.chara_id);
        v["SummonDataVersion"] = jnum(s.version);
        if (n++) list += ",";
        list += render(formats::kSignTemplate, v);
    }
    list += "]";
    return list;
}

std::string FromApi::sign_delete(const Caller& caller, const json::Value& body) {
    const auto id = static_cast<std::uint64_t>(int_of(body, "SummonDataId", 0));
    std::lock_guard<std::mutex> lk(mu_);
    signs_.erase(std::remove_if(signs_.begin(), signs_.end(),
                                [&](const Sign& s) { return s.online_id == caller.online_id && (!id || s.id == id); }),
                 signs_.end());
    return "0";
}

std::string FromApi::sign_request(const Caller& caller, const json::Value& body) {
    const auto target_uid = static_cast<std::uint64_t>(int_of(body, "TargetUserId", 0));
    const auto sign_id = static_cast<std::uint64_t>(int_of(body, "SummonDataId", 0));
    std::string target;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (const Sign& s : signs_) {
            if ((target_uid && s.user_id == target_uid) || (sign_id && s.id == sign_id)) {
                target = s.online_id;
                break;
            }
        }
        if (target.empty() && target_uid) {
            for (const auto& [id, uid] : users_) {
                if (uid == target_uid) target = id;
            }
        }
    }
    if (target.empty() || target == caller.online_id) {
        log("from api: %s asked to summon user %llu: no such sign", caller.online_id.c_str(),
            static_cast<unsigned long long>(target_uid));
        return "0";
    }
    PartyHostService::RoomView room;
    if (!service_.room_of(caller.online_id, &room, true)) {
        log("from api: %s asked to summon %s without a room of its own", caller.online_id.c_str(), target.c_str());
        return "0";
    }
    json::Value host = json::Value::make_object();
    service_.resolve(caller.online_id, &host);
    json::Value ev = json::Value::make_object();
    ev.set("Name", "guest_invite");
    ev.set("RoomId", static_cast<long long>(room.room_id));
    ev.set("HostOnlineId", caller.online_id);
    ev.set("HostAddr", str_of(host, "Addr"));
    ev.set("HostPort", int_of(host, "Port", 0));
    ev.set("HostLocalAddr", str_of(host, "LocalAddr"));
    ev.set("HostLocalPort", int_of(host, "LocalPort", 0));
    ev.set("HostMappedAddr", str_of(host, "MappedAddr"));
    ev.set("HostMappedPort", int_of(host, "MappedPort", 0));
    ev.set("MemberTag", int_of(room.extra, "MemberTag", 1));
    ev.set("HostArea", int_of(room.extra, "HostArea", 0));
    ev.set("HostLevel", int_of(room.extra, "HostLevel", 0));
    json::Value pos = json::Value::make_array();
    const json::Value* hp = room.extra.find("HostPos");
    for (int i = 0; i < 3; ++i) {
        double v = 0;
        if (hp && hp->type == json::Value::Type::Array && static_cast<std::size_t>(i) < hp->array.size())
            v = hp->array[static_cast<std::size_t>(i)].number;
        pos.push(v);
    }
    ev.set("HostPos", std::move(pos));
    service_.push_event(target, std::move(ev));
    log("from api: %s summons %s into room %llu", caller.online_id.c_str(), target.c_str(),
        static_cast<unsigned long long>(room.room_id));
    return "1";
}

}  // namespace bbnet::party
