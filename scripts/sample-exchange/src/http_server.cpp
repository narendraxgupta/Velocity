// =============================================================================
//  http_server.cpp — REST surface of the sample exchange.
//
//  Endpoints
//  ---------
//    GET    /healthz                  liveness
//    POST   /orders                   place limit order  -> 200 with fills[]
//    DELETE /orders/{id}              cancel             -> 204 or 404
//    GET    /book?levels=N            depth snapshot     -> { bids[], asks[] }
//
//  Wire format (POST /orders)
//  --------------------------
//    {
//      "id":       "01HQ8KZRJ2P0Y4VBV7M6E9XQGH",
//      "side":     "BUY" | "SELL",
//      "price":    100050,           // fixed-point
//      "quantity": 100
//    }
// =============================================================================

#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include <drogon/HttpController.h>
#include <drogon/drogon.h>
#include <nlohmann/json.hpp>

#include "sample_exchange/orderbook.h"
#include "velocity/common/log.h"
#include "velocity/common/signals.h"
#include "velocity/common/time.h"

namespace velocity::sample_exchange {

namespace {

// One process-wide instance. drogon::app() is single-instance so this lives
// for the duration of the program.
std::unique_ptr<OrderBook> g_book;

[[nodiscard]] auto parse_side(std::string_view s) -> std::optional<Side> {
    if (s == "BUY")  return Side::BUY;
    if (s == "SELL") return Side::SELL;
    return std::nullopt;
}

[[nodiscard]] auto json_error(drogon::HttpStatusCode code, std::string_view msg) {
    nlohmann::json body{{"error", msg}};
    auto resp = [](std::string _b){ auto _r = drogon::HttpResponse::newHttpResponse(); _r->setContentTypeCode(drogon::CT_APPLICATION_JSON); _r->setBody(_b); return _r; }(body.dump());
    resp->setStatusCode(code);
    return resp;
}

class Routes : public drogon::HttpController<Routes> {
public:
    METHOD_LIST_BEGIN
        ADD_METHOD_TO(Routes::health, "/healthz",      drogon::Get);
        ADD_METHOD_TO(Routes::place,  "/orders",       drogon::Post);
        ADD_METHOD_TO(Routes::cancel, "/orders/{id}",  drogon::Delete);
        ADD_METHOD_TO(Routes::book,   "/book",         drogon::Get);
    METHOD_LIST_END

    auto health(const drogon::HttpRequestPtr&,
                std::function<void(const drogon::HttpResponsePtr&)>&& cb) const -> void {
        cb([](std::string _b){ auto _r = drogon::HttpResponse::newHttpResponse(); _r->setContentTypeCode(drogon::CT_APPLICATION_JSON); _r->setBody(_b); return _r; }(R"({"status":"ok","service":"sample-exchange"})"));
    }

    auto place(const drogon::HttpRequestPtr& req,
               std::function<void(const drogon::HttpResponsePtr&)>&& cb) const -> void {
        if (!g_book) {
            cb(json_error(drogon::k503ServiceUnavailable, "book not initialized"));
            return;
        }
        nlohmann::json j;
        try {
            j = nlohmann::json::parse(req->body());
        } catch (...) {
            cb(json_error(drogon::k400BadRequest, "malformed JSON"));
            return;
        }
        if (!j.contains("id") || !j.contains("side") || !j.contains("price") || !j.contains("quantity")) {
            cb(json_error(drogon::k400BadRequest, "missing field"));
            return;
        }
        const auto side = parse_side(j.value("side", ""));
        if (!side) {
            cb(json_error(drogon::k400BadRequest, "invalid side"));
            return;
        }

        Order o{
            j["id"].get<std::string>(),
            *side,
            j["price"].get<std::int64_t>(),
            j["quantity"].get<std::uint64_t>(),
            velocity::time::realtime_ns(),
        };
        auto outcome = g_book->submit(std::move(o));
        if (!outcome.accepted) {
            cb(json_error(drogon::k400BadRequest, outcome.reject_reason));
            return;
        }

        nlohmann::json fills = nlohmann::json::array();
        for (const auto& f : outcome.fills) {
            fills.push_back({
                {"taker_id",  f.taker_id},
                {"maker_id",  f.maker_id},
                {"price",     f.price},
                {"quantity",  f.quantity},
                {"aggressor", f.aggressor_side == Side::BUY ? "BUY" : "SELL"},
                {"ts_ns",     f.ts_ns},
            });
        }
        nlohmann::json body{
            {"id",                j["id"]},
            {"fills",             fills},
            {"resting_quantity",  outcome.resting_quantity},
        };
        cb([](std::string _b){ auto _r = drogon::HttpResponse::newHttpResponse(); _r->setContentTypeCode(drogon::CT_APPLICATION_JSON); _r->setBody(_b); return _r; }(body.dump()));
    }

    auto cancel(const drogon::HttpRequestPtr&,
                std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                std::string id) const -> void {
        if (!g_book) {
            cb(json_error(drogon::k503ServiceUnavailable, "book not initialized"));
            return;
        }
        if (g_book->cancel(id)) {
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setStatusCode(drogon::k204NoContent);
            cb(resp);
        } else {
            cb(json_error(drogon::k404NotFound, "unknown id"));
        }
    }

    auto book(const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) const -> void {
        if (!g_book) {
            cb(json_error(drogon::k503ServiceUnavailable, "book not initialized"));
            return;
        }
        const auto levels_s = req->getParameter("levels");
        std::size_t levels = 10;
        if (!levels_s.empty()) {
            try {
                levels = std::stoul(levels_s);
            } catch (...) {
                cb(json_error(drogon::k400BadRequest, "invalid levels"));
                return;
            }
        }
        auto [bids, asks] = g_book->snapshot(levels);

        nlohmann::json bid_arr = nlohmann::json::array();
        nlohmann::json ask_arr = nlohmann::json::array();
        for (const auto& l : bids) bid_arr.push_back({{"price", l.price}, {"qty", l.quantity}, {"orders", l.order_count}});
        for (const auto& l : asks) ask_arr.push_back({{"price", l.price}, {"qty", l.quantity}, {"orders", l.order_count}});

        nlohmann::json body{{"bids", bid_arr}, {"asks", ask_arr}};
        cb([](std::string _b){ auto _r = drogon::HttpResponse::newHttpResponse(); _r->setContentTypeCode(drogon::CT_APPLICATION_JSON); _r->setBody(_b); return _r; }(body.dump()));
    }
};

}  // namespace

auto run(std::string listen_host, std::uint16_t listen_port) -> void {
    g_book = std::make_unique<OrderBook>();

    auto& app = drogon::app();
    app.addListener(listen_host, listen_port)
       .setThreadNum(0)
       .setLogPath("")
       .enableServerHeader(false)
       .setMaxConnectionNum(100'000);

    std::thread([&app]() {
        while (!velocity::signals::shutdown_requested()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        app.quit();
    }).detach();

    VLOG_INFO("sample-exchange ready on {}:{}", listen_host, listen_port);
    app.run();
}

}  // namespace velocity::sample_exchange
