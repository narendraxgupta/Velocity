// =============================================================================
//  correctness_validator/validator.h — entry point declarations shared
//  between main.cpp and validator.cpp.
// =============================================================================

#pragma once

#include <cstdint>
#include <string>

namespace velocity::correctness_validator {

struct ValidatorConfig {
    std::string   brokers;
    std::string   group_id;
    std::string   telemetry_topic;
    std::string   fills_topic;
    std::string   questdb_host;
    std::uint16_t questdb_ilp_port;
    std::uint16_t metrics_port;
    // Optional Redis address for mismatch sample LPUSH. Empty disables.
    std::string   redis_addr;
    // Maximum number of mismatch records kept in Redis per submission.
    std::uint32_t mismatches_cap{200};
};

// Blocks until shutdown_requested(). Throws on unrecoverable Kafka errors.
auto run(ValidatorConfig cfg) -> void;

}  // namespace velocity::correctness_validator
