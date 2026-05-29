// =============================================================================
//  tracing.cpp — minimal W3C trace context + OTLP/HTTP exporter.
//
//  We deliberately implement this without the full opentelemetry-cpp SDK.
//  The whole point of the exercise is to prove distributed tracing works
//  across our polyglot stack — a 200-line emitter that posts JSON to
//  Jaeger's OTLP/HTTP endpoint is enough for that proof.
//
//  Wire format: https://opentelemetry.io/docs/specs/otlp/#otlphttp
//  We use the v1 JSON encoding (`application/json` to `/v1/traces`).
// =============================================================================
#include "velocity/common/tracing.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

// libcurl is already in the project's Conan deps (used by the bot REST
// transport). We use the easy synchronous API on a worker thread; this is
// not a hot path.
#include <curl/curl.h>

namespace velocity::common::tracing {

// ----------------------------------------------------------------------------
//  Hex helpers
// ----------------------------------------------------------------------------
namespace {

constexpr char kHex[] = "0123456789abcdef";

template <std::size_t N>
auto bytes_to_hex(const std::array<std::uint8_t, N>& bytes) -> std::string {
    std::string out(N * 2, '0');
    for (std::size_t i = 0; i < N; ++i) {
        out[2 * i + 0] = kHex[(bytes[i] >> 4) & 0x0F];
        out[2 * i + 1] = kHex[(bytes[i] >> 0) & 0x0F];
    }
    return out;
}

auto hex_val(char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

template <std::size_t N>
auto hex_to_bytes(std::string_view hex, std::array<std::uint8_t, N>& out) -> bool {
    if (hex.size() != N * 2) return false;
    for (std::size_t i = 0; i < N; ++i) {
        const int hi = hex_val(hex[2 * i + 0]);
        const int lo = hex_val(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    return true;
}

auto now_unix_ns() -> std::uint64_t {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count());
}

// CSPRNG. Tries /dev/urandom first (cheap, blocking-free), then a
// thread-local mt19937 seeded from random_device.
auto fill_random(void* buf, std::size_t n) -> void {
#if defined(__unix__) || defined(__APPLE__)
    static thread_local std::ifstream urandom("/dev/urandom", std::ios::binary);
    if (urandom && urandom.read(static_cast<char*>(buf), static_cast<std::streamsize>(n))) {
        return;
    }
#endif
    static thread_local std::mt19937_64 prng{std::random_device{}()};
    auto* p = static_cast<std::uint8_t*>(buf);
    for (std::size_t i = 0; i < n; ++i) {
        p[i] = static_cast<std::uint8_t>(prng() & 0xFF);
    }
}

}   // namespace

// ----------------------------------------------------------------------------
//  Context primitives
// ----------------------------------------------------------------------------

auto new_context() -> Context {
    Context c;
    fill_random(c.trace_id.data(), c.trace_id.size());
    fill_random(c.span_id.data(),  c.span_id.size());
    return c;
}

auto to_traceparent(const Context& c) -> std::string {
    // 00-<trace>-<span>-<flags>
    std::string out;
    out.reserve(55);
    out.append("00-");
    out.append(bytes_to_hex(c.trace_id));
    out.append("-");
    out.append(bytes_to_hex(c.span_id));
    out.append("-");
    out.push_back(kHex[(c.flags >> 4) & 0x0F]);
    out.push_back(kHex[(c.flags >> 0) & 0x0F]);
    return out;
}

auto parse_traceparent(std::string_view header) -> std::optional<Context> {
    // version-<32 hex>-<16 hex>-<2 hex>
    if (header.size() < 55) return std::nullopt;
    if (header[2] != '-' || header[35] != '-' || header[52] != '-') return std::nullopt;
    const auto version = header.substr(0, 2);
    if (version != "00") return std::nullopt;

    Context c;
    if (!hex_to_bytes(header.substr(3,  32), c.trace_id)) return std::nullopt;
    if (!hex_to_bytes(header.substr(36, 16), c.span_id))  return std::nullopt;
    const int hi = hex_val(header[53]);
    const int lo = hex_val(header[54]);
    if (hi < 0 || lo < 0) return std::nullopt;
    c.flags = static_cast<std::uint8_t>((hi << 4) | lo);
    return c;
}

// ----------------------------------------------------------------------------
//  Exporter — OTLP/HTTP/JSON batch sender.
// ----------------------------------------------------------------------------
namespace {

struct FinishedSpan {
    std::string                 name;
    Context                     ctx;
    std::optional<SpanId>       parent_span_id;
    std::uint64_t               start_unix_ns{};
    std::uint64_t               end_unix_ns{};
    std::vector<Attribute>      attrs;
    bool                        ok{true};
    std::string                 status_desc;
};

class Exporter {
public:
    static auto instance() -> Exporter& {
        static Exporter ex;
        return ex;
    }

    auto init(std::string service_name, std::string endpoint) -> void {
        std::lock_guard lock(mu_);
        if (running_.load()) return;
        service_ = std::move(service_name);
        endpoint_ = std::move(endpoint);
        if (endpoint_.empty()) {
            spdlog::info("tracing disabled (no OTLP endpoint)");
            return;
        }
        // Strip trailing slash.
        while (!endpoint_.empty() && endpoint_.back() == '/') endpoint_.pop_back();
        traces_url_ = endpoint_ + "/v1/traces";
        running_.store(true);
        worker_ = std::thread(&Exporter::run, this);
        spdlog::info("tracing initialised (service={}, otlp={})", service_, traces_url_);
    }

    auto shutdown() -> void {
        if (!running_.exchange(false)) return;
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
        flush_remaining();
    }

    auto enqueue(FinishedSpan s) -> void {
        if (!running_.load()) return;
        {
            std::lock_guard lock(mu_);
            if (queue_.size() >= kMaxQueue) {
                ++dropped_;
                return;
            }
            queue_.push_back(std::move(s));
        }
        cv_.notify_one();
    }

    [[nodiscard]] auto service() const -> const std::string& { return service_; }

private:
    Exporter() = default;
    ~Exporter() { shutdown(); }

    static constexpr std::size_t kMaxQueue   = 4096;
    static constexpr std::size_t kBatchSize  = 64;
    static constexpr auto        kFlushEvery = std::chrono::seconds(2);

    auto run() -> void {
        while (running_.load()) {
            std::vector<FinishedSpan> batch;
            {
                std::unique_lock lock(mu_);
                cv_.wait_for(lock, kFlushEvery, [this] {
                    return queue_.size() >= kBatchSize || !running_.load();
                });
                const auto take =
                    static_cast<std::ptrdiff_t>(std::min(queue_.size(), kBatchSize));
                batch.insert(batch.end(),
                             std::make_move_iterator(queue_.begin()),
                             std::make_move_iterator(queue_.begin() + take));
                queue_.erase(queue_.begin(), queue_.begin() + take);
            }
            if (!batch.empty()) send(batch);
        }
    }

    auto flush_remaining() -> void {
        std::vector<FinishedSpan> batch;
        {
            std::lock_guard lock(mu_);
            batch.swap(queue_);
        }
        if (!batch.empty()) send(batch);
    }

    auto send(const std::vector<FinishedSpan>& batch) -> void {
        nlohmann::json spans_json = nlohmann::json::array();
        for (const auto& s : batch) spans_json.push_back(span_to_json(s));

        const nlohmann::json body = {
            {"resourceSpans", {{
                {"resource", {
                    {"attributes", {{
                        {"key",   "service.name"},
                        {"value", {{"stringValue", service_}}},
                    }}},
                }},
                {"scopeSpans", {{
                    {"scope", {{"name", "velocity-tracing"}}},
                    {"spans", spans_json},
                }}},
            }}},
        };
        const std::string payload = body.dump();

        CURL* curl = curl_easy_init();
        if (!curl) return;
        struct curl_slist* hdrs = nullptr;
        hdrs = curl_slist_append(hdrs, "Content-Type: application/json");

        curl_easy_setopt(curl, CURLOPT_URL, traces_url_.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(payload.size()));
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 1500L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +[](char*, size_t s, size_t n, void*) {
            return s * n;   // discard body
        });

        const auto res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            // Don't spam — log every 100th failure.
            static std::atomic<std::size_t> err_n{0};
            if ((err_n.fetch_add(1) % 100) == 0) {
                spdlog::warn("tracing export failed: {}", curl_easy_strerror(res));
            }
        }
        curl_slist_free_all(hdrs);
        curl_easy_cleanup(curl);
    }

    static auto span_to_json(const FinishedSpan& s) -> nlohmann::json {
        nlohmann::json attrs = nlohmann::json::array();
        for (const auto& a : s.attrs) {
            attrs.push_back({
                {"key",   a.key},
                {"value", {{"stringValue", a.value}}},
            });
        }
        nlohmann::json span = {
            {"traceId",           bytes_to_hex(s.ctx.trace_id)},
            {"spanId",            bytes_to_hex(s.ctx.span_id)},
            {"name",              s.name},
            {"kind",              2},                       // SPAN_KIND_SERVER
            {"startTimeUnixNano", std::to_string(s.start_unix_ns)},
            {"endTimeUnixNano",   std::to_string(s.end_unix_ns)},
            {"attributes",        attrs},
            {"status", {
                {"code",    s.ok ? 1 : 2},                  // OK or ERROR
                {"message", s.status_desc},
            }},
        };
        if (s.parent_span_id) {
            span["parentSpanId"] = bytes_to_hex(*s.parent_span_id);
        }
        return span;
    }

    std::mutex                  mu_;
    std::condition_variable     cv_;
    std::vector<FinishedSpan>   queue_;
    std::thread                 worker_;
    std::atomic_bool            running_{false};
    std::size_t                 dropped_{0};
    std::string                 service_;
    std::string                 endpoint_;
    std::string                 traces_url_;
};

}   // namespace

// ----------------------------------------------------------------------------
//  Span impl
// ----------------------------------------------------------------------------

Span::Span(std::string name, Context ctx, std::optional<SpanId> parent)
    : name_(std::move(name))
    , ctx_(ctx)
    , parent_span_id_(parent)
    , start_unix_ns_(now_unix_ns())
{}

Span::Span(Span&& other) noexcept
    : name_           (std::move(other.name_))
    , ctx_            (other.ctx_)
    , parent_span_id_ (std::move(other.parent_span_id_))
    , start_unix_ns_  (other.start_unix_ns_)
    , attrs_          (std::move(other.attrs_))
    , ok_             (other.ok_)
    , status_desc_    (std::move(other.status_desc_))
    , active_         (other.active_)
{
    // The source no longer owns the export — its destructor is a no-op.
    other.active_ = false;
}

Span& Span::operator=(Span&& other) noexcept {
    if (this == &other) return *this;
    // End the existing span before re-binding.
    end();
    name_           = std::move(other.name_);
    ctx_            = other.ctx_;
    parent_span_id_ = std::move(other.parent_span_id_);
    start_unix_ns_  = other.start_unix_ns_;
    attrs_          = std::move(other.attrs_);
    ok_             = other.ok_;
    status_desc_    = std::move(other.status_desc_);
    active_         = other.active_;
    other.active_   = false;
    return *this;
}

Span::~Span() {
    end();
}

auto Span::end() -> void {
    if (!active_) return;
    active_ = false;
    Exporter::instance().enqueue({
        .name           = std::move(name_),
        .ctx            = ctx_,
        .parent_span_id = parent_span_id_,
        .start_unix_ns  = start_unix_ns_,
        .end_unix_ns    = now_unix_ns(),
        .attrs          = std::move(attrs_),
        .ok             = ok_,
        .status_desc    = std::move(status_desc_),
    });
}

auto Span::set_attribute(std::string key, std::string value) -> void {
    if (!active_) return;
    attrs_.push_back({.key = std::move(key), .value = std::move(value)});
}

auto Span::set_status(bool ok, std::string description) -> void {
    if (!active_) return;
    ok_ = ok;
    status_desc_ = std::move(description);
}

// ----------------------------------------------------------------------------
//  Process tracer
// ----------------------------------------------------------------------------

auto init(std::string service_name, std::string otlp_endpoint) -> void {
    Exporter::instance().init(std::move(service_name), std::move(otlp_endpoint));
}

auto shutdown() -> void {
    Exporter::instance().shutdown();
}

auto start_span(std::string name, std::optional<Context> parent) -> Span {
    if (parent && parent->is_valid()) {
        Context child = *parent;
        const auto parent_span = parent->span_id;
        fill_random(child.span_id.data(), child.span_id.size());
        return Span(std::move(name), child, parent_span);
    }
    return Span(std::move(name), new_context(), std::nullopt);
}

}   // namespace velocity::common::tracing
