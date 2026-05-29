# =============================================================================
#  VelocityCompileOptions
#
#  A single INTERFACE target carrying our compile / link / warning policy.
#  Every Velocity target should link against `velocity::compile_options`
#  publicly so the policy propagates to consumers.
# =============================================================================

if(TARGET velocity_compile_options)
    return()
endif()

add_library(velocity_compile_options INTERFACE)
add_library(velocity::compile_options ALIAS velocity_compile_options)

# -----------------------------------------------------------------------------
#  Standard discipline
# -----------------------------------------------------------------------------
target_compile_features(velocity_compile_options INTERFACE cxx_std_20)

# -----------------------------------------------------------------------------
#  Warnings — strict but pragmatic
# -----------------------------------------------------------------------------
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(velocity_compile_options INTERFACE
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wnon-virtual-dtor
        -Wold-style-cast
        -Wcast-align
        -Wunused
        -Woverloaded-virtual
        -Wconversion
        -Wsign-conversion
        -Wnull-dereference
        -Wdouble-promotion
        -Wformat=2
        -Wimplicit-fallthrough
        -Wno-missing-field-initializers   # protobuf-generated code
    )

    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(velocity_compile_options INTERFACE
            -Wmisleading-indentation
            -Wduplicated-cond
            -Wduplicated-branches
            -Wlogical-op
            -Wuseless-cast
        )
    endif()

    if(VELOCITY_WARNINGS_AS_ERRORS)
        target_compile_options(velocity_compile_options INTERFACE -Werror)
    endif()
endif()

# -----------------------------------------------------------------------------
#  Optimization knobs
# -----------------------------------------------------------------------------
if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    target_compile_options(velocity_compile_options INTERFACE
        $<$<COMPILE_LANGUAGE:CXX>:-O3>
        $<$<COMPILE_LANGUAGE:CXX>:-fno-omit-frame-pointer>
    )

    if(VELOCITY_USE_LTO)
        include(CheckIPOSupported)
        check_ipo_supported(RESULT _ipo_supported OUTPUT _ipo_error)
        if(_ipo_supported)
            set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE PARENT_SCOPE)
            message(STATUS "LTO enabled.")
        else()
            message(WARNING "LTO requested but not supported: ${_ipo_error}")
        endif()
    endif()
endif()

# -----------------------------------------------------------------------------
#  Native CPU — opt-in via VELOCITY_NATIVE=ON for prod builds on the target
#  machine. Off by default because it breaks portability of binaries.
# -----------------------------------------------------------------------------
option(VELOCITY_NATIVE "Build with -march=native for the host CPU" OFF)
if(VELOCITY_NATIVE)
    target_compile_options(velocity_compile_options INTERFACE -march=native -mtune=native)
endif()

# -----------------------------------------------------------------------------
#  Linker hardening
# -----------------------------------------------------------------------------
target_link_options(velocity_compile_options INTERFACE
    LINKER:-z,relro
    LINKER:-z,now
    LINKER:--as-needed
)

# -----------------------------------------------------------------------------
#  Prefer lld / mold when available — substantial link-time speedup.
# -----------------------------------------------------------------------------
find_program(_velocity_mold mold)
find_program(_velocity_lld  ld.lld)
if(_velocity_mold)
    target_link_options(velocity_compile_options INTERFACE -fuse-ld=mold)
    message(STATUS "Using mold linker.")
elseif(_velocity_lld)
    target_link_options(velocity_compile_options INTERFACE -fuse-ld=lld)
    message(STATUS "Using lld linker.")
endif()
