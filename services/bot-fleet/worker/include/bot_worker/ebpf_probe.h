// =============================================================================
//  bot_worker/ebpf_probe.h
//
//  Userspace half of the kernel-level TCP latency probe. Loads the compiled
//  BPF object (`latency_probe.bpf.o`, generated from
//  worker/ebpf/latency_probe.bpf.c by clang), attaches the kprobes, and
//  drains a perf-buffer on a dedicated thread, updating an HdrHistogram of
//  kernel-observed round-trip times.
//
//  Build modes:
//
//    * VELOCITY_ENABLE_EBPF=ON  — links against libbpf, includes the real
//                                 implementation, and embeds the .bpf.o.
//    * VELOCITY_ENABLE_EBPF=OFF — compiles a no-op stub. The worker still
//                                 instantiates an `EbpfProbe` but every
//                                 method is a fast inline noop. This lets
//                                 us ship the same binary on macOS dev
//                                 machines and on production EKS nodes
//                                 with Linux ≥ 5.4.
//
//  Threading: the drain thread is owned by EbpfProbe. start() spawns it,
//  stop() joins it. snapshot_percentiles() is lock-free (per-CPU buckets
//  feeding into a single-writer HdrHistogram → polled from any thread).
// =============================================================================

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

namespace velocity::bot_worker {

// Snapshot of the kernel-side latency distribution at a point in time.
// All fields are nanoseconds.
struct KernelLatencySnapshot {
    std::uint64_t samples_observed{0};
    std::uint64_t p50_ns{0};
    std::uint64_t p90_ns{0};
    std::uint64_t p99_ns{0};
    std::uint64_t p999_ns{0};
    std::uint64_t max_ns{0};
};

class EbpfProbe {
public:
    EbpfProbe() noexcept;
    ~EbpfProbe();

    EbpfProbe(const EbpfProbe&)            = delete;
    EbpfProbe& operator=(const EbpfProbe&) = delete;

    // start() returns false if eBPF is disabled (build flag) or if kernel
    // attach fails (insufficient capabilities, kernel too old, etc.).
    // Callers should NOT treat failure as fatal — userspace timing is
    // still available; the kernel series simply remains zeroed.
    [[nodiscard]] auto start() -> bool;

    // Stop the drain thread + detach the kprobes. Safe to call multiple
    // times; safe to call from any thread.
    auto stop() noexcept -> void;

    // Tell the probe which PIDs to record. By default the probe records
    // only the bot-worker PID; the controller may want to extend this to
    // child processes spawned by some persona implementations.
    auto allow_pid(std::uint32_t pid) -> void;

    // Live distribution snapshot. Lock-free; safe to call at the
    // publisher's emit cadence (~250 ms in production).
    [[nodiscard]] auto snapshot() const noexcept -> KernelLatencySnapshot;

    // Whether the probe is actively recording kernel events. False if
    // start() returned false or if stop() has been called.
    [[nodiscard]] auto is_active() const noexcept -> bool;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Helper: true iff the build was configured with VELOCITY_ENABLE_EBPF=ON
// AND we're running on Linux. Used by the bot-worker bootstrap to log
// whether kernel timings will be available at all.
[[nodiscard]] auto ebpf_supported() noexcept -> bool;

}  // namespace velocity::bot_worker
