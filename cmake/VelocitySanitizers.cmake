# =============================================================================
#  VelocitySanitizers
#
#  Hooks AddressSanitizer / UndefinedBehaviorSanitizer / ThreadSanitizer into
#  the build via velocity::compile_options. Sanitizers are mutually
#  exclusive — ASAN and TSAN cannot be combined.
# =============================================================================

if(NOT TARGET velocity_compile_options)
    message(FATAL_ERROR "Include VelocityCompileOptions before VelocitySanitizers.")
endif()

if(VELOCITY_ENABLE_ASAN AND VELOCITY_ENABLE_TSAN)
    message(FATAL_ERROR
        "ASAN and TSAN are mutually exclusive. Pick one.")
endif()

if(VELOCITY_ENABLE_ASAN)
    message(STATUS "AddressSanitizer enabled.")
    target_compile_options(velocity_compile_options INTERFACE
        -fsanitize=address
        -fno-omit-frame-pointer
        -fno-optimize-sibling-calls
    )
    target_link_options(velocity_compile_options INTERFACE -fsanitize=address)
endif()

if(VELOCITY_ENABLE_TSAN)
    message(STATUS "ThreadSanitizer enabled.")
    target_compile_options(velocity_compile_options INTERFACE -fsanitize=thread)
    target_link_options(velocity_compile_options INTERFACE    -fsanitize=thread)
endif()

if(VELOCITY_ENABLE_UBSAN)
    message(STATUS "UndefinedBehaviorSanitizer enabled.")
    target_compile_options(velocity_compile_options INTERFACE
        -fsanitize=undefined
        -fno-sanitize-recover=all
    )
    target_link_options(velocity_compile_options INTERFACE -fsanitize=undefined)
endif()
