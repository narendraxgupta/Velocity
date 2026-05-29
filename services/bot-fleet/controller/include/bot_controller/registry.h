// =============================================================================
//  bot_controller/registry.h
//
//  The worker registry is owned jointly by:
//    * BotControlImpl     — populates the registry as workers connect.
//    * BenchmarkServiceImpl — reads it to fan out LoadPlans and to aggregate
//                              per-worker StatusUpdate counters into a
//                              BenchmarkSnapshot.
//
//  Concurrency: a shared_mutex guards the session map. Writes happen on
//  every worker connect/disconnect (rare); reads happen on every
//  benchmark snapshot tick (4 Hz per active benchmark) and every fan-out
//  (once per benchmark start). The shared_mutex therefore strictly improves
//  on a plain mutex.
// =============================================================================

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "bot.grpc.pb.h"
#include "bot.pb.h"

#include "bot_controller/load_planner.h"

namespace velocity::bot_controller {

// One entry per active worker stream.
struct WorkerSession {
    std::string                              worker_id;
    std::string                              hostname;
    std::uint32_t                            cpu_count{1};
    grpc::ServerReaderWriter<velocity::bot::v1::ControllerToWorker,
                             velocity::bot::v1::WorkerToController>* stream{nullptr};

    // Serializes writes to the worker stream from controller-side threads
    // (BotControl receiver thread + benchmark fan-out thread).
    std::mutex                               write_mu;

    // Latest StatusUpdate counters (cumulative since worker boot, not since
    // benchmark start — BenchmarkSession diffs against the recorded baseline).
    std::atomic<std::uint64_t>               last_sent_total{0};
    std::atomic<std::uint64_t>               last_acked_total{0};
    std::atomic<std::uint64_t>               last_errored_total{0};
    std::atomic<std::uint64_t>               last_current_rps{0};
    std::atomic<std::uint32_t>               last_active_bots{0};
    std::atomic<std::uint32_t>               last_cpu_percent{0};
    std::atomic<std::int64_t>                last_heartbeat_ns{0};

    // Kernel-side latency, sourced from the worker's eBPF probe (see
    // platform/services/bot-fleet/worker/ebpf/latency_probe.bpf.c). Zero
    // means the worker never attached the probe — the controller hides
    // the kernel series on the wire when no worker reports samples.
    std::atomic<std::uint64_t>               last_kernel_p50_ns{0};
    std::atomic<std::uint64_t>               last_kernel_p99_ns{0};
    std::atomic<std::uint64_t>               last_kernel_p999_ns{0};
    std::atomic<std::uint64_t>               last_kernel_samples{0};
};

class Registry {
public:
    auto add(std::shared_ptr<WorkerSession> ws) -> void {
        std::unique_lock lk(mu_);
        sessions_[ws->worker_id] = std::move(ws);
    }
    auto remove(const std::string& id) -> void {
        std::unique_lock lk(mu_);
        sessions_.erase(id);
    }
    auto capacities() const -> std::vector<WorkerCapacity> {
        std::shared_lock lk(mu_);
        std::vector<WorkerCapacity> out;
        out.reserve(sessions_.size());
        for (const auto& [_, s] : sessions_) {
            out.push_back(WorkerCapacity{s->worker_id, s->cpu_count});
        }
        return out;
    }
    auto session(const std::string& id) const -> std::shared_ptr<WorkerSession> {
        std::shared_lock lk(mu_);
        auto it = sessions_.find(id);
        return it == sessions_.end() ? nullptr : it->second;
    }
    auto all() const -> std::vector<std::shared_ptr<WorkerSession>> {
        std::shared_lock lk(mu_);
        std::vector<std::shared_ptr<WorkerSession>> out;
        out.reserve(sessions_.size());
        for (const auto& [_, s] : sessions_) out.push_back(s);
        return out;
    }
    [[nodiscard]] auto size() const -> std::size_t {
        std::shared_lock lk(mu_);
        return sessions_.size();
    }

private:
    mutable std::shared_mutex                                              mu_;
    std::unordered_map<std::string, std::shared_ptr<WorkerSession>>        sessions_;
};

}  // namespace velocity::bot_controller
