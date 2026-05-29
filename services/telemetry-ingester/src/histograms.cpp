// =============================================================================
//  histograms.cpp — per-submission HdrHistogram management.
//
//  Configuration choices:
//    * lowest discernible value = 100 ns   (we never measure faster than that)
//    * highest trackable value  = 60 sec   (room for the worst-case timeout)
//    * 3 significant figures              (~600 bytes encoded per histogram)
//
//  These three parameters define ~2,000 buckets — enough to render a
//  meaningful CDF without inflating memory.
// =============================================================================

#include "telemetry_ingester/histograms.h"

#include <stdexcept>

#include <hdr_histogram.h>

namespace velocity::telemetry_ingester {

namespace {

constexpr std::int64_t kLowest  = 100LL;
constexpr std::int64_t kHighest = 60LL * 1'000'000'000LL;
constexpr int          kSigFig  = 3;

[[nodiscard]] auto make_hg() -> hdr_histogram* {
    hdr_histogram* h = nullptr;
    const auto rc = hdr_init(kLowest, kHighest, kSigFig, &h);
    if (rc != 0 || h == nullptr) {
        throw std::runtime_error("hdr_init failed");
    }
    return h;
}

}  // namespace

void HistogramStore::HgDeleter::operator()(hdr_histogram* h) const noexcept {
    if (h) hdr_close(h);
}

HistogramStore::HistogramStore()  = default;
HistogramStore::~HistogramStore() = default;

auto HistogramStore::record(const std::string& submission_id,
                            std::int64_t latency_ns) -> void {
    if (latency_ns < kLowest) latency_ns = kLowest;
    if (latency_ns > kHighest) latency_ns = kHighest;

    auto it = map_.find(submission_id);
    if (it == map_.end()) {
        it = map_.emplace(submission_id,
                          std::unique_ptr<hdr_histogram, HgDeleter>{make_hg()}).first;
    }
    hdr_record_value(it->second.get(), latency_ns);
}

auto HistogramStore::reset(hdr_histogram* h) -> void {
    if (h) hdr_reset(h);
}

}  // namespace velocity::telemetry_ingester
