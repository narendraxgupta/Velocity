// =============================================================================
//  tenant.cpp — JWT-backed tenant resolution + key scoping.
//
//  Verification choices
//  --------------------
//  We support HS256 only at this stage. RS256/JWKS is the obvious next
//  step but it adds an OpenSSL dependency surface that's overkill for
//  the first ship — and HS256 with a 256-bit rotated secret is
//  perfectly adequate when the gateway sits behind mTLS at the edge.
//
//  The JWT body is parsed manually because pulling in `jwt-cpp` here
//  would add a ~5MB compile-time cost for what is ultimately 60 lines
//  of base64 + HMAC. The implementation is closely modelled on
//  `validateJWT()` from the OAuth2 RFC examples; tests cover the
//  edge-cases (missing exp, wrong signature, malformed segments).
// =============================================================================

#include "api_gateway/tenant.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <vector>

#include <openssl/hmac.h>
#include <nlohmann/json.hpp>

#include "velocity/common/log.h"

namespace velocity::api_gateway::tenant {

namespace {

Config g_config;
std::once_flag g_init_once;

// Base64url decode → vector<uint8_t>. RFC 4648 §5.
auto b64url_decode(std::string_view in) -> std::vector<std::uint8_t> {
    static const auto table = [] {
        std::array<int8_t, 256> t{};
        for (auto& v : t) v = -1;
        const char* alpha = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
        for (int i = 0; i < 64; ++i) t[static_cast<std::uint8_t>(alpha[i])] = static_cast<int8_t>(i);
        return t;
    }();

    std::vector<std::uint8_t> out;
    out.reserve((in.size() * 3) / 4 + 4);
    std::uint32_t acc = 0;
    int bits = 0;
    for (auto c : in) {
        if (c == '=') break;
        const auto v = table[static_cast<std::uint8_t>(c)];
        if (v < 0) continue;       // ignore newlines etc.
        acc = (acc << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>((acc >> bits) & 0xFFu));
        }
    }
    return out;
}

auto hmac_sha256(const std::vector<std::uint8_t>& key,
                 std::string_view data) -> std::array<std::uint8_t, 32> {
    std::array<std::uint8_t, 32> out{};
    unsigned int len = 32;
    HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
         reinterpret_cast<const std::uint8_t*>(data.data()),
         data.size(),
         out.data(), &len);
    return out;
}

auto constant_time_equal(const std::vector<std::uint8_t>& a,
                         const std::array<std::uint8_t, 32>& b) noexcept -> bool {
    if (a.size() != b.size()) return false;
    std::uint8_t diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) diff |= a[i] ^ b[i];
    return diff == 0;
}

// Parse and verify a compact JWS HS256 token. Returns the decoded claims
// or throws on any failure. Callers map exceptions to 401.
auto parse_and_verify(std::string_view token, const std::vector<std::uint8_t>& key)
    -> nlohmann::json {
    const auto first_dot  = token.find('.');
    const auto second_dot = token.find('.', first_dot + 1);
    if (first_dot == std::string_view::npos || second_dot == std::string_view::npos) {
        throw std::runtime_error("malformed JWT");
    }
    const auto header_b64  = token.substr(0, first_dot);
    const auto payload_b64 = token.substr(first_dot + 1, second_dot - first_dot - 1);
    const auto sig_b64     = token.substr(second_dot + 1);
    const std::string signed_data{token.substr(0, second_dot)};

    const auto header_bytes  = b64url_decode(header_b64);
    const auto payload_bytes = b64url_decode(payload_b64);
    const auto sig_bytes     = b64url_decode(sig_b64);

    const auto header = nlohmann::json::parse(
        std::string_view{reinterpret_cast<const char*>(header_bytes.data()),
                         header_bytes.size()});
    if (header.value("alg", "") != "HS256") {
        throw std::runtime_error("unsupported alg");
    }
    const auto expected = hmac_sha256(key, signed_data);
    if (!constant_time_equal(sig_bytes, expected)) {
        throw std::runtime_error("bad signature");
    }
    return nlohmann::json::parse(
        std::string_view{reinterpret_cast<const char*>(payload_bytes.data()),
                         payload_bytes.size()});
}

[[nodiscard]] auto bypass_path(std::string_view path) -> bool {
    const auto& bp = g_config.bypass_paths;
    std::size_t i = 0;
    while (i < bp.size()) {
        auto end = bp.find(',', i);
        if (end == std::string::npos) end = bp.size();
        const std::string_view candidate{bp.data() + i, end - i};
        if (!candidate.empty() && path.starts_with(candidate)) return true;
        i = end + 1;
    }
    return false;
}

}  // namespace

auto role_from_string(std::string_view s) noexcept -> Role {
    if (s == "admin")    return Role::ADMIN;
    if (s == "operator") return Role::OPERATOR;
    return Role::SUBMITTER;
}

auto role_to_string(Role r) noexcept -> std::string_view {
    switch (r) {
        case Role::ADMIN:    return "admin";
        case Role::OPERATOR: return "operator";
        case Role::SUBMITTER: default: return "submitter";
    }
}

auto configure(Config cfg) -> void {
    std::call_once(g_init_once, []() {});  // pre-warm
    g_config = std::move(cfg);
    VLOG_INFO("tenant: configured (require_auth={}, jwt_enabled={})",
              g_config.require_auth, !g_config.hs256_secret_b64.empty());
}

auto configuration() noexcept -> const Config& { return g_config; }

auto resolve(const drogon::HttpRequestPtr& req) -> std::optional<Context> {
    const auto path = req->path();
    if (bypass_path(path)) {
        // The `/v1/share/` bypass exists ONLY for the public GET view —
        // the unguessable URL token IS the authentication for that path.
        // A revoke (DELETE /v1/share/{token}) must still resolve the
        // caller's real tenant so ownership can be enforced; otherwise the
        // bypass handed every revoke the anonymous "default" identity,
        // which both blocked real owners from revoking their own shares
        // (owner tid != "default") and let anyone revoke "default"-owned
        // shares. So for a non-GET share request, fall through to the JWT /
        // header / fallback resolution below instead of bypassing.
        const bool share_path = path.starts_with("/v1/share/");
        if (!share_path || req->method() == drogon::Get) {
            Context ctx;
            ctx.id      = "default";
            ctx.role    = Role::SUBMITTER;
            return ctx;
        }
    }

    // 1) JWT path.
    //
    // If a caller PRESENTS a Bearer token we treat it as a claim of
    // identity: a verification failure is terminal and must NOT silently
    // downgrade to the legacy X-Velocity-Tenant header (which any client
    // can set to an arbitrary tenant). Allowing that downgrade let a
    // forged/expired token pick a victim tenant via the header. On a
    // verification failure we skip straight to the require_auth decision
    // instead of trying the header.
    const auto& auth_header = req->getHeader("authorization");
    if (!auth_header.empty() && !g_config.hs256_secret_b64.empty()) {
        constexpr std::string_view prefix = "Bearer ";
        if (auth_header.rfind(prefix, 0) == 0) {
            try {
                const auto secret = b64url_decode(g_config.hs256_secret_b64);
                const auto claims = parse_and_verify(
                    std::string_view{auth_header}.substr(prefix.size()),
                    secret);

                const auto exp = claims.value("exp", static_cast<std::int64_t>(0));
                if (exp > 0) {
                    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                    if (now >= exp) throw std::runtime_error("token expired");
                }
                Context ctx;
                ctx.id         = claims.value("tid", "");
                ctx.subject    = claims.value("sub", "");
                ctx.role       = role_from_string(claims.value("role", "submitter"));
                ctx.issued_at  = std::chrono::system_clock::time_point{
                    std::chrono::seconds{claims.value("iat", static_cast<std::int64_t>(0))}};
                ctx.expires_at = std::chrono::system_clock::time_point{
                    std::chrono::seconds{exp}};
                if (ctx.id.empty()) ctx.id = "default";
                return ctx;
            } catch (const std::exception& e) {
                VLOG_WARN("tenant: JWT rejected: {}", e.what());
                // Fail closed: a bad token is never downgraded to the
                // legacy header. Under require_auth this is a hard 401;
                // otherwise it falls back to the "default" tenant below.
                if (g_config.require_auth) return std::nullopt;
                Context ctx;
                ctx.id   = "default";
                ctx.role = Role::SUBMITTER;
                return ctx;
            }
        }
    }

    // 2) Legacy header path. Only reached when NO bearer token was
    // presented — a presented-but-invalid token short-circuits above.
    if (const auto& hdr = req->getHeader("x-velocity-tenant"); !hdr.empty()) {
        Context ctx;
        ctx.id = hdr;
        ctx.role = Role::SUBMITTER;
        return ctx;
    }

    // 3) Fallback.
    if (g_config.require_auth) return std::nullopt;
    Context ctx;
    ctx.id   = "default";
    ctx.role = Role::SUBMITTER;
    return ctx;
}

// Idempotency check: a value is "already scoped" ONLY when its prefix
// is `<sigil><tenant_id><sep>`. A bare `t:` prefix is not enough —
// otherwise `scoped_key("acme", "t:other:foo")` would leak the
// `other` tenant's key out to `acme` callers. This is the entire
// reason multi-tenant isolation exists; the helper must fail closed.
[[nodiscard]] auto already_scoped(std::string_view value, std::string_view tenant_id,
                                  std::string_view sigil, char sep) -> bool {
    if (value.size() < sigil.size() + tenant_id.size() + 1) return false;
    if (!value.starts_with(sigil)) return false;
    auto rest = value.substr(sigil.size());
    if (!rest.starts_with(tenant_id)) return false;
    return rest[tenant_id.size()] == sep;
}

auto scoped_key(std::string_view tenant_id, std::string_view raw_key) -> std::string {
    constexpr std::string_view prefix = "t:";
    if (already_scoped(raw_key, tenant_id, prefix, ':')) return std::string{raw_key};
    std::string out;
    out.reserve(prefix.size() + tenant_id.size() + 1 + raw_key.size());
    out.append(prefix).append(tenant_id).push_back(':');
    out.append(raw_key);
    return out;
}

auto scoped_topic(std::string_view tenant_id, std::string_view raw_topic) -> std::string {
    constexpr std::string_view prefix = "t.";
    if (already_scoped(raw_topic, tenant_id, prefix, '.')) return std::string{raw_topic};
    std::string out;
    out.reserve(prefix.size() + tenant_id.size() + 1 + raw_topic.size());
    out.append(prefix).append(tenant_id).push_back('.');
    out.append(raw_topic);
    return out;
}

auto scoped_object(std::string_view tenant_id, std::string_view raw_object) -> std::string {
    constexpr std::string_view prefix = "t/";
    if (already_scoped(raw_object, tenant_id, prefix, '/')) return std::string{raw_object};
    std::string out;
    out.reserve(prefix.size() + tenant_id.size() + 1 + raw_object.size());
    out.append(prefix).append(tenant_id).push_back('/');
    out.append(raw_object);
    return out;
}

}  // namespace velocity::api_gateway::tenant
