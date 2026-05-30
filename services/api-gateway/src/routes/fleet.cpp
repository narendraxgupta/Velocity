// =============================================================================
//  GET /v1/fleet — current bot-worker registry snapshot.
//
//  The bot-controller periodically writes a JSON list to Redis key
//  `fleet:workers`. We just relay it. This avoids opening a third gRPC
//  call into a singleton bot-controller process from the gateway's hot
//  request path, and gives operators a fast cache-friendly endpoint.
// =============================================================================

#include <chrono>
#include <string>

#include <drogon/HttpController.h>
#include <nlohmann/json.hpp>
#include <sw/redis++/redis++.h>

#include "api_gateway/clients.h"
#include "velocity/common/log.h"

namespace velocity::api_gateway::routes {

class Fleet : public drogon::HttpController<Fleet> {
public:
    METHOD_LIST_BEGIN
        ADD_METHOD_TO(Fleet::list, "/v1/fleet", drogon::Get);
    METHOD_LIST_END

    auto list(const drogon::HttpRequestPtr& /*req*/,
              std::function<void(const drogon::HttpResponsePtr&)>&& callback) const -> void {
        auto* redis = clients::RedisClient::get();
        if (!redis) {
            auto r = [](std::string _b){ auto _r = drogon::HttpResponse::newHttpResponse(); _r->setContentTypeCode(drogon::CT_APPLICATION_JSON); _r->setBody(_b); return _r; }(
                R"({"workers":[],"error":"redis_unavailable"})");
            r->setStatusCode(drogon::k503ServiceUnavailable);
            callback(r);
            return;
        }
        try {
            const auto val = redis->get("fleet:workers");
            const auto body = val.value_or("{\"workers\":[]}");
            auto r = [](std::string _b){ auto _r = drogon::HttpResponse::newHttpResponse(); _r->setContentTypeCode(drogon::CT_APPLICATION_JSON); _r->setBody(_b); return _r; }(body);
            r->addHeader("Cache-Control", "no-cache");
            callback(r);
        } catch (const std::exception& e) {
            VLOG_WARN("fleet redis read failed: {}", e.what());
            auto r = [](std::string _b){ auto _r = drogon::HttpResponse::newHttpResponse(); _r->setContentTypeCode(drogon::CT_APPLICATION_JSON); _r->setBody(_b); return _r; }(
                R"({"workers":[],"error":"redis_failed"})");
            r->setStatusCode(drogon::k500InternalServerError);
            callback(r);
        }
    }
};

}  // namespace velocity::api_gateway::routes
