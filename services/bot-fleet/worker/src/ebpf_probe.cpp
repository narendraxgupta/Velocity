// =============================================================================
//  ebpf_probe.cpp — userspace half of the kernel-level latency probe.
//
//  Real implementation when VELOCITY_ENABLE_EBPF=ON; no-op stub otherwise.
//  See header for the contract.
// =============================================================================

#include "bot_worker/ebpf_probe.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include <hdr/hdr_histogram.h>

#include "velocity/common/log.h"

#if defined(VELOCITY_ENABLE_EBPF) && VELOCITY_ENABLE_EBPF
#  include <bpf/libbpf.h>
#  include <bpf/bpf.h>
#  include <unistd.h>
#  include <sys/resource.h>
   // The skeleton header is produced by `bpftool gen skeleton` at build time
   // from latency_probe.bpf.o; the CMake rule below regenerates it on every
   // BPF source change. Falls back to a missing-symbol error if the build
   // didn't actually compile the BPF object.
#  include "latency_probe.skel.h"
#endif

namespace velocity::bot_worker {

namespace {

// HDR config: 1 ns lo bound, 1 second hi, 3 significant figures.
constexpr std::int64_t kHdrLowestNs  = 1;
constexpr std::int64_t kHdrHighestNs = 1'000'000'000LL;
constexpr int          kHdrSigFigs   = 3;

// Per-CPU staging buffer drained by the consumer thread. We use a single
// HDR histogram protected by a mutex rather than per-thread shadows because
// the lock is held for nanoseconds (HDR record is two cache-line accesses)
// and snapshot reads are infrequent (≈ 4 Hz from the publisher).
struct Distribution {
    mutable std::mutex   m;
    hdr_histogram*       h{nullptr};
    std::atomic_uint64_t samples{0};

    Distribution() {
        hdr_init(kHdrLowestNs, kHdrHighestNs, kHdrSigFigs, &h);
    }
    ~Distribution() { if (h) hdr_close(h); }

    auto record(std::uint64_t ns) -> void {
        std::lock_guard lk{m};
        hdr_record_value(h, static_cast<std::int64_t>(ns));
        samples.fetch_add(1, std::memory_order_relaxed);
    }

    auto snapshot() const noexcept -> KernelLatencySnapshot {
        KernelLatencySnapshot out;
        std::lock_guard lk{m};
        out.samples_observed = samples.load(std::memory_order_relaxed);
        out.p50_ns  = static_cast<std::uint64_t>(hdr_value_at_percentile(h, 50.0));
        out.p90_ns  = static_cast<std::uint64_t>(hdr_value_at_percentile(h, 90.0));
        out.p99_ns  = static_cast<std::uint64_t>(hdr_value_at_percentile(h, 99.0));
        out.p999_ns = static_cast<std::uint64_t>(hdr_value_at_percentile(h, 99.9));
        out.max_ns  = static_cast<std::uint64_t>(hdr_max(h));
        return out;
    }
};

}  // namespace

// =============================================================================
//  Impl — implementation pImpl
// =============================================================================

#if defined(VELOCITY_ENABLE_EBPF) && VELOCITY_ENABLE_EBPF

struct EbpfProbe::Impl {
    Distribution           dist;
    latency_probe_bpf*     skel{nullptr};
    perf_buffer*           pb{nullptr};
    std::thread            drainer;
    std::atomic_bool       running{false};

    // Bump RLIMIT_MEMLOCK so libbpf can lock the BPF maps in RAM. On
    // modern kernels (≥5.11) maps use the bpf-cgroup accounting path
    // and this is a no-op; on older kernels it's required.
    static auto raise_memlock() -> void {
        struct rlimit r{ RLIM_INFINITY, RLIM_INFINITY };
        (void)setrlimit(RLIMIT_MEMLOCK, &r);
    }

    static auto handle_event(void* ctx, int /*cpu*/, void* data, __u32 size) -> void {
        if (size < sizeof(struct latency_event)) return;
        const auto* ev = static_cast<const struct latency_event*>(data);
        auto* self = static_cast<Impl*>(ctx);
        if (ev->delta_ns > 0) self->dist.record(ev->delta_ns);
    }

    static auto handle_lost(void* /*ctx*/, int cpu, __u64 lost_cnt) -> void {
        VLOG_WARN("ebpf: perf buffer lost {} events on cpu {}", lost_cnt, cpu);
    }

    auto start() -> bool {
        raise_memlock();

        skel = latency_probe_bpf__open();
        if (!skel) {
            VLOG_WARN("ebpf: skeleton open failed");
            return false;
        }
        if (latency_probe_bpf__load(skel) != 0) {
            VLOG_WARN("ebpf: BPF object load failed (kernel mismatch / capability denied)");
            latency_probe_bpf__destroy(skel);
            skel = nullptr;
            return false;
        }
        if (latency_probe_bpf__attach(skel) != 0) {
            VLOG_WARN("ebpf: kprobe attach failed");
            latency_probe_bpf__destroy(skel);
            skel = nullptr;
            return false;
        }

        // Default: allow our own PID + permissive mode (key=0) for tests.
        const std::uint32_t my_pid = static_cast<std::uint32_t>(getpid());
        const std::uint8_t  on = 1;
        const std::uint32_t zero = 0;
        bpf_map__update_elem(skel->maps.pid_filter, &my_pid, sizeof(my_pid),
                             &on, sizeof(on), BPF_ANY);
        bpf_map__update_elem(skel->maps.pid_filter, &zero, sizeof(zero),
                             &on, sizeof(on), BPF_ANY);

        pb = perf_buffer__new(bpf_map__fd(skel->maps.events),
                              /*page_cnt=*/16,
                              handle_event,
                              handle_lost,
                              this,
                              nullptr);
        if (!pb) {
            VLOG_WARN("ebpf: perf_buffer__new failed");
            latency_probe_bpf__destroy(skel);
            skel = nullptr;
            return false;
        }

        running.store(true, std::memory_order_release);
        drainer = std::thread([this] {
            while (running.load(std::memory_order_acquire)) {
                // 100ms timeout — keeps shutdown responsive while not
                // burning CPU during idle phases.
                int err = perf_buffer__poll(pb, 100);
                if (err < 0 && err != -EINTR) {
                    VLOG_WARN("ebpf: perf_buffer poll error: {}", err);
                    break;
                }
            }
        });

        VLOG_INFO("ebpf: kernel latency probe armed (pid={})", my_pid);
        return true;
    }

    auto stop() noexcept -> void {
        if (!running.exchange(false, std::memory_order_acq_rel)) return;
        if (drainer.joinable()) drainer.join();
        if (pb)  { perf_buffer__free(pb); pb = nullptr; }
        if (skel) { latency_probe_bpf__destroy(skel); skel = nullptr; }
    }

    auto allow_pid(std::uint32_t pid) -> void {
        if (!skel) return;
        const std::uint8_t on = 1;
        bpf_map__update_elem(skel->maps.pid_filter, &pid, sizeof(pid),
                             &on, sizeof(on), BPF_ANY);
    }
};

[[nodiscard]] auto ebpf_supported() noexcept -> bool { return true; }

#else  // VELOCITY_ENABLE_EBPF off — compile a no-op stub

struct EbpfProbe::Impl {
    Distribution         dist;
    std::atomic_bool     running{false};

    auto start() -> bool { return false; }
    auto stop() noexcept -> void { running.store(false); }
    auto allow_pid(std::uint32_t) -> void {}
};

[[nodiscard]] auto ebpf_supported() noexcept -> bool { return false; }

#endif

// =============================================================================
//  Public dispatch
// =============================================================================

EbpfProbe::EbpfProbe() noexcept : impl_(std::make_unique<Impl>()) {}
EbpfProbe::~EbpfProbe() { stop(); }

auto EbpfProbe::start() -> bool {
    if (!impl_) return false;
    return impl_->start();
}

auto EbpfProbe::stop() noexcept -> void {
    if (impl_) impl_->stop();
}

auto EbpfProbe::allow_pid(std::uint32_t pid) -> void {
    if (impl_) impl_->allow_pid(pid);
}

auto EbpfProbe::snapshot() const noexcept -> KernelLatencySnapshot {
    if (!impl_) return {};
    return impl_->dist.snapshot();
}

auto EbpfProbe::is_active() const noexcept -> bool {
    if (!impl_) return false;
    return impl_->running.load(std::memory_order_acquire);
}

}  // namespace velocity::bot_worker
