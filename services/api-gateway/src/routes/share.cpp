// =============================================================================
//  Public sharing — POST /v1/share + GET /v1/share/{token}
//
//  Goals
//  -----
//  - Owner of a submission can mint a short, opaque token that
//    anyone (no JWT, no account) can GET to see a frozen snapshot
//    of one benchmark run.
//  - The snapshot is REDACTED — we strip subject/email/IP fields and
//    keep only:
//      * team display name
//      * benchmark profile, phase, RPS, latency percentiles, score
//      * leaderboard rank at mint time
//  - Tokens are one-way: a token can be revoked but not "edited".
//    Revoking deletes the Redis key; the snapshot is preserved if
//    the issuer wants to mint a fresh token later.
//
//  RBAC
//  ----
//  - POST /v1/share              → SUBMITTER (must own the submission)
//  - DELETE /v1/share/{token}    → SUBMITTER (must own the submission)
//  - GET /v1/share/{token}       → BYPASSED in tenant.cpp (no auth)
//
//  Wire format
//  -----------
//      Key:   share:token:<token>     → JSON snapshot
//      Key:   share:by_sub:<submission_id> → SET<token>  (for revocation)
//      TTL:   30 days default; configurable per share via "ttl_seconds"
// =============================================================================

#include <chrono>
#include <cstdlib>
#include <iterator>
#include <random>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <drogon/HttpController.h>
#include <nlohmann/json.hpp>
#include <sw/redis++/redis++.h>

#include "api_gateway/audit.h"
#include "api_gateway/clients.h"
#include "api_gateway/rbac.h"
#include "api_gateway/tenant.h"
#include "velocity/common/log.h"

namespace velocity::api_gateway::routes {

namespace {

constexpr std::int64_t DEFAULT_TTL_SECONDS = 30 * 24 * 60 * 60;

[[nodiscard]] auto mint_token() -> std::string {
    // 22-char URL-safe token (~132 bits). Plenty for the
    // "anyone with the URL" threat model.
    static constexpr char alpha[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::uniform_int_distribution<int> dist(0, 63);
    std::string out;
    out.resize(22);
    for (auto& c : out) c = alpha[dist(rng)];
    return out;
}

[[nodiscard]] auto redacted_snapshot(const nlohmann::json& raw) -> nlohmann::json {
    nlohmann::json out = nlohmann::json::object();
    out["team"]             = raw.value("team", "");
    out["display"]          = raw.value("display", "");
    out["profile"]          = raw.value("profile", "");
    out["composite_score"]  = raw.value("composite_score", 0.0);
    out["latency_ns"]       = raw.value("latency_ns", nlohmann::json::object());
    out["throughput_rps"]   = raw.value("throughput_rps", 0.0);
    out["rank_at_mint"]     = raw.value("rank", 0);
    out["snapshot_at_ns"]   = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return out;
}

}  // namespace

class Share : public drogon::HttpController<Share> {
public:
    METHOD_LIST_BEGIN
        METHOD_ADD(Share::mint,   "/v1/share",          drogon::Post);
        METHOD_ADD(Share::view,   "/v1/share/{token}",  drogon::Get);
        METHOD_ADD(Share::revoke, "/v1/share/{token}",  drogon::Delete);
    METHOD_LIST_END

    auto mint(const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) const
        -> void {
        auto ctx = rbac::context_from(req);
        if (!ctx) {
            cb(json_error(drogon::k401Unauthorized, "unauthenticated"));
            return;
        }
        const auto body = nlohmann::json::parse(std::string{req->getBody()},
                                                nullptr, false);
        if (body.is_discarded()) {
            cb(json_error(drogon::k400BadRequest, "invalid JSON"));
            return;
        }
        const auto submission_id = body.value("submission_id", "");
        const auto benchmark_id  = body.value("benchmark_id", "");
        const auto ttl_seconds   = body.value("ttl_seconds", DEFAULT_TTL_SECONDS);
        if (submission_id.empty() || benchmark_id.empty()) {
            cb(json_error(drogon::k400BadRequest, "submission_id and benchmark_id required"));
            return;
        }

        auto* r = clients::RedisClient::get();
        if (r == nullptr) {
            cb(json_error(drogon::k503ServiceUnavailable, "redis unavailable"));
            return;
        }

        // Fetch the canonical snapshot from the leaderboard cache —
        // scoring-service writes this when a benchmark finishes.
        const auto snapshot_key = tenant::scoped_key(ctx->id,
            "leaderboard:snapshot:" + submission_id + ":" + benchmark_id);
        nlohmann::json snapshot;
        try {
            const auto raw = r->get(snapshot_key);
            if (!raw) {
                cb(json_error(drogon::k404NotFound, "snapshot not found"));
                return;
            }
            snapshot = nlohmann::json::parse(*raw, nullptr, false);
            if (snapshot.is_discarded()) {
                cb(json_error(drogon::k500InternalServerError, "snapshot malformed"));
                return;
            }
        } catch (const std::exception& e) {
            cb(json_error(drogon::k500InternalServerError, std::string{"redis: "} + e.what()));
            return;
        }

        const auto token = mint_token();
        const auto redacted = redacted_snapshot(snapshot);
        try {
            r->setex(std::string{"share:token:"} + token,
                     std::chrono::seconds{ttl_seconds},
                     redacted.dump());
            // Owner metadata. We keep this OUT of the share:token:
            // value because the value is served unauthenticated to
            // anyone with the URL — leaking the tenant id there
            // would unmask the team behind the share. The meta key
            // is only ever read on the revoke path, which is
            // authenticated.
            const auto meta_key = std::string{"share:meta:"} + token;
            using Pair = std::pair<std::string, std::string>;
            std::vector<Pair> meta{
                {"tenant_id",     ctx->id},
                {"submission_id", submission_id},
                {"benchmark_id",  benchmark_id},
                {"subject",       ctx->subject},
            };
            r->hmset(meta_key, meta.begin(), meta.end());
            r->expire(meta_key, std::chrono::seconds{ttl_seconds + 86400});
            r->sadd(std::string{"share:by_sub:"} + submission_id, token);
            r->expire(std::string{"share:by_sub:"} + submission_id,
                      std::chrono::seconds{ttl_seconds + 86400});
        } catch (const std::exception& e) {
            cb(json_error(drogon::k500InternalServerError,
                          std::string{"redis: "} + e.what()));
            return;
        }

        audit::Event ev;
        ev.tenant_id    = ctx->id;
        ev.subject      = ctx->subject;
        ev.role         = std::string{tenant::role_to_string(ctx->role)};
        ev.action       = "share.mint";
        ev.resource_type= "benchmark";
        ev.resource_id  = benchmark_id;
        ev.outcome      = audit::Outcome::ALLOW;
        ev.status_code  = 201;
        audit::emit(ev);

        nlohmann::json out{
            {"token", token},
            {"url",   "/share/" + token},
            {"ttl_seconds", ttl_seconds},
        };
        auto resp = drogon::HttpResponse::newHttpJsonResponse(out.dump());
        resp->setStatusCode(drogon::k201Created);
        cb(resp);
    }

    auto view(const drogon::HttpRequestPtr& /*req*/,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb,
              const std::string& token) const -> void {
        // NB: this route is on the bypass list in tenant.cpp — anyone
        // can GET it with just the URL. The token's 22 chars (132 bits)
        // are the entire authn mechanism.
        auto* r = clients::RedisClient::get();
        if (r == nullptr) {
            cb(json_error(drogon::k503ServiceUnavailable, "redis unavailable"));
            return;
        }
        const auto raw = r->get("share:token:" + token);
        if (!raw) {
            cb(json_error(drogon::k404NotFound, "share not found or expired"));
            return;
        }
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(drogon::k200OK);
        resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        // Cache aggressively — snapshots are immutable.
        resp->addHeader("Cache-Control", "public, max-age=86400, immutable");
        resp->setBody(*raw);
        cb(resp);
    }

    auto revoke(const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                const std::string& token) const -> void {
        auto ctx = rbac::context_from(req);
        if (!ctx) {
            cb(json_error(drogon::k401Unauthorized, "unauthenticated"));
            return;
        }
        auto* r = clients::RedisClient::get();
        if (r == nullptr) {
            cb(json_error(drogon::k503ServiceUnavailable, "redis unavailable"));
            return;
        }

        // Verify ownership before deleting. Without this check, any
        // user with ANY tenant's JWT could revoke any token they
        // happen to know the URL of (e.g. one posted publicly).
        // ADMINs of the same gateway deployment can still revoke
        // cross-tenant — that's the explicit privilege of the role.
        const auto meta_key = std::string{"share:meta:"} + token;
        std::string owner_tid;
        try {
            std::unordered_map<std::string, std::string> meta;
            r->hgetall(meta_key, std::inserter(meta, meta.begin()));
            if (auto it = meta.find("tenant_id"); it != meta.end()) {
                owner_tid = it->second;
            }
        } catch (const std::exception& e) {
            VLOG_WARN("share.revoke: meta read failed: {}", e.what());
        }
        if (owner_tid.empty()) {
            // Either a pre-meta-tracking share, OR the token doesn't
            // exist at all. We can't tell the difference cheaply, so
            // fall back to 404 — better than leaking the existence of
            // arbitrary tokens via a 403/204 distinction.
            cb(json_error(drogon::k404NotFound, "share not found"));
            return;
        }
        if (owner_tid != ctx->id && ctx->role != tenant::Role::ADMIN) {
            // Audit the cross-tenant attempt — this is the kind of
            // signal a SOC team wants to see.
            audit::Event denied{};
            denied.tenant_id    = ctx->id;
            denied.subject      = ctx->subject;
            denied.role         = std::string{tenant::role_to_string(ctx->role)};
            denied.action       = "share.revoke";
            denied.resource_type= "share";
            denied.resource_id  = token;
            denied.outcome      = audit::Outcome::DENY;
            denied.status_code  = 403;
            audit::emit(denied);
            cb(json_error(drogon::k403Forbidden, "not the owner"));
            return;
        }

        r->del("share:token:" + token);
        r->del(meta_key);
        audit::Event ev;
        ev.tenant_id    = ctx->id;
        ev.subject      = ctx->subject;
        ev.role         = std::string{tenant::role_to_string(ctx->role)};
        ev.action       = "share.revoke";
        ev.resource_type= "share";
        ev.resource_id  = token;
        ev.outcome      = audit::Outcome::ALLOW;
        ev.status_code  = 204;
        audit::emit(ev);
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(drogon::k204NoContent);
        cb(resp);
    }

private:
    static auto json_error(drogon::HttpStatusCode code, std::string_view msg)
        -> drogon::HttpResponsePtr {
        nlohmann::json body{{"error", std::string{msg}}};
        auto resp = drogon::HttpResponse::newHttpJsonResponse(body.dump());
        resp->setStatusCode(code);
        return resp;
    }
};

}  // namespace velocity::api_gateway::routes
