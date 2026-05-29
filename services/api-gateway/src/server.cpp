// =============================================================================
//  server.cpp — Drogon-backed HTTP/WS facade.
//
//  This file is intentionally small. Route handlers live in src/routes/*.cpp;
//  the server here only wires the framework together. Phase 2 expands this
//  with the WebSocket controller for live benchmark streams and the
//  Submission/Bot gRPC stubs.
// =============================================================================

#include "api_gateway/server.h"

#include <memory>

#include <drogon/HttpAppFramework.h>
#include <drogon/drogon.h>

#include "api_gateway/audit.h"
#include "api_gateway/clients.h"
#include "api_gateway/rbac.h"
#include "api_gateway/tenant.h"
#include "velocity/common/log.h"
#include "velocity/common/signals.h"

#include <nlohmann/json.hpp>

namespace velocity::api_gateway {

struct Server::Impl {
    ServerConfig cfg;
    explicit Impl(ServerConfig c) : cfg(std::move(c)) {}
};

Server::Server(ServerConfig cfg) : impl_(std::make_unique<Impl>(std::move(cfg))) {}
Server::~Server() = default;

auto Server::run() -> void {
    clients::GrpcClients::init(impl_->cfg.submission_engine_grpc,
                               impl_->cfg.bot_controller_grpc);
    clients::RedisClient::init(impl_->cfg.redis_addr);

    // Configure the tenant / JWT layer from env so operators can flip
    // require-auth on without rebuilding the image. Defaults are
    // permissive (require_auth=false, "default" tenant) so dev loops
    // keep working unchanged.
    tenant::Config tcfg;
    if (const auto* sec = std::getenv("VELOCITY_JWT_HS256_SECRET_B64"); sec && *sec) {
        tcfg.hs256_secret_b64 = sec;
    }
    if (const auto* req = std::getenv("VELOCITY_AUTH_REQUIRE"); req && *req) {
        tcfg.require_auth = std::string_view{req} == "true" ||
                            std::string_view{req} == "1";
    }
    if (const auto* bp = std::getenv("VELOCITY_AUTH_BYPASS_PATHS"); bp && *bp) {
        tcfg.bypass_paths = bp;
    }
    tenant::configure(std::move(tcfg));

    auto& app = drogon::app();

    app.addListener(impl_->cfg.listen_host, impl_->cfg.http_port)
       .setThreadNum(0)                            // 0 = one thread per core
       .setLogPath("")                             // we route through spdlog
       .enableServerHeader(false)
       .setIdleConnectionTimeout(60)
       .registerSyncAdvice([](const drogon::HttpRequestPtr& req)
                                -> drogon::HttpResponsePtr {
           VLOG_DEBUG("{} {}", req->methodString(), req->path());
           // Resolve tenant ONCE per request and stash on the
           // attributes so route handlers don't re-parse the JWT.
           auto tctx = tenant::resolve(req);
           if (!tctx) {
               nlohmann::json body{{"error", "authentication required"}};
               auto resp = drogon::HttpResponse::newHttpJsonResponse(body.dump());
               resp->setStatusCode(drogon::k401Unauthorized);
               resp->addHeader("WWW-Authenticate", "Bearer");
               return resp;
           }
           auto attrs = req->attributes();
           if (attrs) {
               attrs->insert("velocity.tenant_id", tctx->id);
               attrs->insert("velocity.tenant_role",
                             std::string{tenant::role_to_string(tctx->role)});
               attrs->insert("velocity.tenant_subject", tctx->subject);
           }
           return drogon::HttpResponsePtr{};
       })
       .registerSyncAdvice([](const drogon::HttpRequestPtr& req)
                                -> drogon::HttpResponsePtr {
           // RBAC layer runs after tenant resolution. By this point a
           // request either has a tenant context attached or has
           // already been short-circuited (bypass path); in both cases
           // we still want to enforce role minimums, e.g. the chaos
           // endpoints stay operator+ even on the bypass list.
           auto tctx    = rbac::context_from(req);
           const auto decision = rbac::check(tctx.get(),
                                             req->methodString(),
                                             req->path());
           if (decision == rbac::Decision::ALLOW) return {};
           VLOG_INFO("rbac: {} {} → {}",
                     req->methodString(), req->path(),
                     decision == rbac::Decision::DENY_UNAUTHENTICATED
                         ? "401" : "403");
           // Audit every denied write attempt — this is the data the
           // SOC team needs to investigate "did anyone try?" questions.
           if (req->methodString() != std::string{"GET"} &&
               req->methodString() != std::string{"HEAD"} &&
               req->methodString() != std::string{"OPTIONS"}) {
               audit::Event ev{};
               ev.tenant_id    = tctx ? tctx->id      : std::string{"unknown"};
               ev.subject      = tctx ? tctx->subject : std::string{};
               ev.role         = tctx
                   ? std::string{tenant::role_to_string(tctx->role)}
                   : std::string{};
               ev.action       = std::string{req->methodString()} + ":" +
                                 std::string{req->path()};
               ev.resource_type= "http";
               ev.resource_id  = std::string{req->path()};
               ev.outcome      = decision == rbac::Decision::DENY_UNAUTHENTICATED
                                     ? audit::Outcome::DENY
                                     : audit::Outcome::DENY;
               ev.status_code  = decision == rbac::Decision::DENY_UNAUTHENTICATED
                                     ? 401 : 403;
               ev.remote_ip    = req->peerAddr().toIp();
               audit::emit(ev);
           }
           return rbac::deny_response(decision);
       });

    // The handlers in `routes/` self-register via Drogon's controller
    // mechanism (DROGON_REGISTER_*) — no explicit wiring needed here.

    // Spawn a watcher thread that flips Drogon's shutdown when our shared
    // signal token flips.
    std::thread shutdown_watcher([&app]() {
        while (!velocity::signals::shutdown_requested()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        VLOG_INFO("shutdown signal received; stopping HTTP server");
        app.quit();
    });
    shutdown_watcher.detach();

    VLOG_INFO("HTTP ready on {}:{}", impl_->cfg.listen_host, impl_->cfg.http_port);
    app.run();
}

auto Server::stop() noexcept -> void {
    drogon::app().quit();
}

}  // namespace velocity::api_gateway
