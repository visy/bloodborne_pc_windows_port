// SPDX-License-Identifier: GPL-3.0-or-later
// FromApi: the FROM game server's HTTP API, answered by the party host in-process
// (docs/PARTY_COOP_PLAN.md A3). The game reaches it at
//   https://<...>.scej-network.jp/.../ss.info   the server-status document (XML <ss>), which
//                                                sends the game's API calls to our gameurl
//   <gameurl>/basic_utils/{login,get_datetime,get_normal_notice,get_emergency_notice,
//                          get_user_agreement,sync_chara_id}
//   <gameurl>/summon_messenger/{create,get,delete,request}   (the party sign board)
//   https://bb-playlog-{test,prod}.s3.amazonaws.com/...       (play logs: PUT, answered 200)
// and everything else under gameurl (messages, bloodstains, ghosts, Chalice channels) gets an
// empty success. Bodies and answers whose exact format is not confirmed live in one table,
// from_api_formats.inc. BB_PARTY_TRACE=1 logs every request and answer.
//
// The sign board is party-only by construction (only party members reach the host): a
// guest's create puts its sign up, the host's get lists every other member's signs, and the
// host's request queues a guest_invite (party_transport.h) for the sign's owner with the
// host's room and addresses - what bbhost's np_matching2.cpp on_server_event reads.
#pragma once

#include "json.h"
#include "party_host_service.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace bbnet::party {

struct HttpRequest {
    std::string method = "GET";  // GET, POST, PUT, HEAD
    std::string url;
    std::vector<std::string> headers;  // "Name: value"
    std::string body;
};

struct HttpResponse {
    int status = 0;  // 0: not answered (the caller reports a network error)
    std::string content_type;
    std::string body;
};

// The fixed host name of our gameurl (ss.info points the game at http://<it>:18671).
constexpr const char* kGameHost = "bbparty.invalid";

class FromApi {
public:
    explicit FromApi(PartyHostService& service);

    // True for the URLs FromApi answers (the FROM hosts, the play-log buckets, our gameurl).
    static bool routes(const std::string& url);
    // Answers one request from `caller` (the host's own online id for its game; a guest's for
    // a request that came over PartyLink).
    void handle(const Caller& caller, const HttpRequest& rq, HttpResponse& out);
    // The same as the "http" transport call's JSON (party_transport.h).
    void handle_json(const Caller& caller, const json::Value& rq, json::Value& reply);

    // The sign board (tests, status).
    struct Sign {
        std::uint64_t id = 0;
        std::uint64_t user_id = 0;
        std::string online_id;
        std::uint32_t area = 0;
        int region = 0;
        int summon_type = 0;
        std::string data_b64;
        json::Value request;  // the create body as sent
    };
    std::vector<Sign> signs() const;
    std::uint64_t user_id_of(const std::string& online_id);

    // The process's (the host's), over PartyHostService::instance().
    static FromApi& instance();

private:
    std::string dispatch(const Caller& caller, const std::string& path, const json::Value& body, int* status,
                         std::string* content_type);
    std::string sign_create(const Caller& caller, const json::Value& body);
    std::string sign_get(const Caller& caller, const json::Value& body);
    std::string sign_delete(const Caller& caller, const json::Value& body);
    std::string sign_request(const Caller& caller, const json::Value& body);

    PartyHostService& service_;
    mutable std::mutex mu_;
    std::vector<Sign> signs_;
    std::uint64_t next_sign_ = 1;
    std::vector<std::pair<std::string, std::uint64_t>> users_;  // online id -> UserId
    std::uint64_t next_user_ = 1001;
};

// The ss.info document as FromApi serves it (tests).
std::string from_api_ss_info();

}  // namespace bbnet::party
