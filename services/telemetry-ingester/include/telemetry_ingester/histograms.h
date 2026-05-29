// =============================================================================
//  telemetry_ingester/histograms.h
//
//  Per-submission HdrHistogram aggregator. Records honest latency
//  (ack_received_ts_ns - intended_ts_ns) so coordinated-omission corrected
//  numbers flow through the entire pipeline.
//
//  Histograms are flushed and reset every `flush_interval_ms` — the
//  resulting LatencyBucket is published to `metrics.latency.1s` for the
//  leaderboard service to consume.
// =============================================================================

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

struct hdr_histogram;

namespace velocity::telemetry_ingester {

class HistogramStore {
public:
    HistogramStore();
    ~HistogramStore();

    HistogramStore(const HistogramStore&)            = delete;
    HistogramStore& operator=(const HistogramStore&) = delete;

    // Record one latency sample for the given submission.
    auto record(const std::string& submission_id, std::int64_t latency_ns) -> void;

    // Snapshot all per-submission histograms; resets them for the next window.
    // The callback is invoked once per submission with (submission_id, hdr*),
    // where hdr is owned by the store and valid only for the duration of the
    // call.
    template <typename F>
    auto drain_for_each(F&& visitor) -> void {
        for (auto& [sid, hg] : map_) {
            if (!hg) continue;
            visitor(sid, hg.get());
            reset(hg.get());
        }
    }

    [[nodiscard]] auto submission_count() const noexcept -> std::size_t {
        return map_.size();
    }

private:
    static auto reset(hdr_histogram* h) -> void;

    struct HgDeleter { auto operator()(hdr_histogram*) const noexcept -> void; };
    std::unordered_map<std::string, std::unique_ptr<hdr_histogram, HgDeleter>> map_;
};

}  // namespace velocity::telemetry_ingester
