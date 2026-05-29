// =============================================================================
//  velocity::common::tracing
//
//  A tiny, dependency-light tracing layer. We do *not* pull in the full
//  opentelemetry-cpp SDK here — it's gigantic and most of our services
//  only need three things:
//
//    1. Parse / generate W3C `traceparent` headers (16-byte trace_id +
//       8-byte span_id + 1-byte flags, per
//       https://www.w3.org/TR/trace-context/).
//    2. Inject the current context into outbound HTTP / gRPC metadata
//       so the next hop joins the same trace.
//    3. Best-effort export of spans to an OTLP/HTTP endpoint
//       (`http://jaeger:4318/v1/traces`). Spans are batched in a
//       background thread; if Jaeger is down, we drop quietly.
//
//  This file is header-only for the part that just touches IDs; the
//  exporter implementation lives in `tracing.cpp`.
// =============================================================================
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace velocity::common::tracing {

// ----------------------------------------------------------------------------
//  W3C trace context primitives
// ----------------------------------------------------------------------------

using TraceId = std::array<std::uint8_t, 16>;
using SpanId  = std::array<std::uint8_t, 8>;

struct Context {
    TraceId          trace_id{};
    SpanId           span_id{};
    std::uint8_t     flags{0x01};        // sampled

    [[nodiscard]] auto is_valid() const noexcept -> bool {
        for (auto b : trace_id) if (b != 0) return true;
        return false;
    }
};

// Generate a fresh 128-bit trace id + 64-bit span id from /dev/urandom or
// platform CSPRNG. Falls back to a thread-local PRNG if the OS source is
// unavailable (e.g. in air-gapped sandboxes).
[[nodiscard]] auto new_context() -> Context;

// Hex-encode the W3C `traceparent` header value:
//   `00-<32 hex trace_id>-<16 hex span_id>-<2 hex flags>`
[[nodiscard]] auto to_traceparent(const Context& c) -> std::string;

// Parse a `traceparent` header. Returns nullopt on any format error;
// callers should usually then start a new context.
[[nodiscard]] auto parse_traceparent(std::string_view header) -> std::optional<Context>;

// ----------------------------------------------------------------------------
//  Span
//
//  RAII guard: constructing a Span records the start time, destruction
//  records the end and pushes it to the exporter's batch queue. Child
//  spans inherit trace_id and set parent_span_id.
// ----------------------------------------------------------------------------

struct Attribute {
    std::string         key;
    std::string         value;   // we stringify everything for simplicity
};

class Span {
public:
    Span(std::string name, Context ctx, std::optional<SpanId> parent = std::nullopt);
    ~Span();

    Span(const Span&)            = delete;
    Span& operator=(const Span&) = delete;

    // Move construction transfers ownership of the "should I emit on
    // destruction" responsibility. The moved-from instance is left in a
    // valid but inert state: its destructor is a no-op. Without this
    // discipline, every move would cause a duplicate (junk) span to be
    // exported when the temporary went out of scope.
    Span(Span&& other) noexcept;
    Span& operator=(Span&& other) noexcept;

    auto set_attribute(std::string key, std::string value) -> void;
    auto set_status(bool ok, std::string description = {}) -> void;

    // Mark the span finished early. After this call the destructor will not
    // export anything. Idempotent.
    auto end() -> void;

    [[nodiscard]] auto context() const noexcept -> const Context& { return ctx_; }
    [[nodiscard]] auto name()    const noexcept -> std::string_view { return name_; }

private:
    std::string                 name_;
    Context                     ctx_;
    std::optional<SpanId>       parent_span_id_;
    std::uint64_t               start_unix_ns_{0};
    std::vector<Attribute>      attrs_;
    bool                        ok_{true};
    std::string                 status_desc_;
    bool                        active_{true};
};

// ----------------------------------------------------------------------------
//  Process-wide tracer
// ----------------------------------------------------------------------------

// Initialise the tracer with the service name (`api-gateway`, ...) and the
// OTLP/HTTP endpoint, e.g. `http://jaeger:4318`. Empty endpoint disables
// export — useful in unit tests. Safe to call multiple times; the first
// call wins.
auto init(std::string service_name, std::string otlp_endpoint) -> void;

// Stop the background exporter, flushing any pending batch. Called from
// signal handlers.
auto shutdown() -> void;

// Start a span. The returned Span is RAII: destruction enqueues it.
[[nodiscard]] auto start_span(std::string name,
                              std::optional<Context> parent = std::nullopt) -> Span;

}   // namespace velocity::common::tracing
