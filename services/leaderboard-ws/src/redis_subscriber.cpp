// =============================================================================
//  redis_subscriber.cpp — Phase 1 stub.
//
//  Phase 2: spawn a thread that owns a sw::redis::Redis subscriber, PSUBSCRIBE
//  to `leaderboard.*`, and on each message hand the payload over to the uWS
//  app's defer() queue for safe publication on the event loop thread.
// =============================================================================

namespace velocity::leaderboard_ws {
[[maybe_unused]] auto _redis_subscriber_link() noexcept -> int { return 0; }
}  // namespace velocity::leaderboard_ws
