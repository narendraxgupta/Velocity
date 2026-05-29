// =============================================================================
//  rl_policy.cpp — ONNX Runtime adapter (gated on VELOCITY_HAS_ONNXRUNTIME).
//
//  Two implementations live side-by-side in this file:
//
//    1) The "real" ORT path — used when the build dependency is wired.
//       It owns a Ort::Session built with a single CPU execution
//       provider (we explicitly do not enable CUDA here; sandboxed
//       bot-workers don't get GPUs). The session config is tuned for
//       low-latency inference: intra_op = 1, inter_op = 1, no graph
//       optimisation (the policy is tiny and the savings flip-flop
//       with ORT versions; we prefer determinism).
//
//    2) A stub path — when ONNX Runtime is unavailable. `load()`
//       returns nullptr; callers fall back to the rule-based
//       market_maker persona. This keeps the worker buildable in
//       constrained CI environments and on machines without the ORT
//       SDK installed.
//
//  Performance budget
//  ------------------
//  Target: ≤ 5µs per inference at 1k bots × 100Hz = 100k inferences/s
//  per worker. The shipped policy is two FC layers of 64 units;
//  measured locally at ~1.2µs/call on a tuned x86 box.
// =============================================================================

#include "bot_worker/rl_policy.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>

#ifdef VELOCITY_HAS_ONNXRUNTIME
#include <onnxruntime_cxx_api.h>
#endif

namespace velocity::bot_worker {

#ifdef VELOCITY_HAS_ONNXRUNTIME

// ---- real ORT implementation -----------------------------------------------

struct RLPolicy::Impl {
    Ort::Env        env{ORT_LOGGING_LEVEL_WARNING, "velocity-rl"};
    Ort::SessionOptions opts;
    std::unique_ptr<Ort::Session> session;
    Ort::AllocatorWithDefaultOptions allocator;

    // ORT requires input/output names; we capture them at load time so
    // hot-path code doesn't allocate.
    std::string input_name;
    std::string output_logits_name;
    std::string output_spread_name;

    static auto from_file(const std::string& onnx_path) -> std::unique_ptr<Impl> {
        // Sanity check: bail out cleanly if the file isn't present.
        if (std::ifstream f{onnx_path}; !f.good()) return nullptr;

        auto impl = std::make_unique<Impl>();
        impl->opts.SetIntraOpNumThreads(1);
        impl->opts.SetInterOpNumThreads(1);
        impl->opts.SetGraphOptimizationLevel(ORT_DISABLE_ALL);

        try {
            impl->session = std::make_unique<Ort::Session>(
                impl->env, onnx_path.c_str(), impl->opts);
        } catch (const std::exception&) {
            return nullptr;
        }

        const auto input_count  = impl->session->GetInputCount();
        const auto output_count = impl->session->GetOutputCount();
        if (input_count < 1 || output_count < 2) return nullptr;

        // SBL3's onnx export uses default names; cache them.
        auto in_alloc  = impl->session->GetInputNameAllocated(0, impl->allocator);
        impl->input_name = in_alloc.get();
        auto out0 = impl->session->GetOutputNameAllocated(0, impl->allocator);
        auto out1 = impl->session->GetOutputNameAllocated(1, impl->allocator);
        impl->output_logits_name = out0.get();
        impl->output_spread_name = out1.get();
        return impl;
    }
};

auto RLPolicy::load(const std::string& onnx_path) -> std::shared_ptr<RLPolicy> {
    auto impl = Impl::from_file(onnx_path);
    if (!impl) return nullptr;
    return std::shared_ptr<RLPolicy>(new RLPolicy(std::move(impl)));
}

auto RLPolicy::infer(const RLObservation& obs) const noexcept -> RLAction {
    if (!impl_ || !impl_->session) {
        return RLAction{RLAction::Kind::NOOP, 1.0F, 0.5F};
    }
    try {
        std::array<float, 12> in{};
        std::memcpy(in.data(), &obs, sizeof(obs));

        const std::array<int64_t, 2> shape{1, 12};
        Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator,
                                                        OrtMemTypeDefault);
        Ort::Value input = Ort::Value::CreateTensor<float>(
            mem, in.data(), in.size(), shape.data(), shape.size());

        std::array<const char*, 1> in_names{impl_->input_name.c_str()};
        std::array<const char*, 2> out_names{
            impl_->output_logits_name.c_str(),
            impl_->output_spread_name.c_str(),
        };

        auto outputs = impl_->session->Run(Ort::RunOptions{nullptr},
                                           in_names.data(),  &input, 1,
                                           out_names.data(), out_names.size());
        if (outputs.size() < 2) {
            return RLAction{RLAction::Kind::NOOP, 1.0F, 0.5F};
        }
        const auto* logits = outputs[0].GetTensorData<float>();
        const auto* sprd   = outputs[1].GetTensorData<float>();
        const int amax = static_cast<int>(std::distance(
            logits, std::max_element(logits, logits + 3)));
        return RLAction{
            static_cast<RLAction::Kind>(std::clamp(amax, 0, 2)),
            std::clamp(sprd[0], 0.0F, 8.0F),
            std::clamp(sprd[1], 0.0F, 1.0F),
        };
    } catch (...) {
        return RLAction{RLAction::Kind::NOOP, 1.0F, 0.5F};
    }
}

auto RLPolicy::enabled() const noexcept -> bool {
    return impl_ != nullptr && impl_->session != nullptr;
}

#else  // !VELOCITY_HAS_ONNXRUNTIME

// ---- stub implementation ----------------------------------------------------

struct RLPolicy::Impl { };

auto RLPolicy::load(const std::string& /*onnx_path*/) -> std::shared_ptr<RLPolicy> {
    return nullptr;
}

auto RLPolicy::infer(const RLObservation& /*obs*/) const noexcept -> RLAction {
    return RLAction{RLAction::Kind::NOOP, 1.0F, 0.5F};
}

auto RLPolicy::enabled() const noexcept -> bool { return false; }

#endif

RLPolicy::RLPolicy(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

RLPolicy::~RLPolicy() = default;

}  // namespace velocity::bot_worker
