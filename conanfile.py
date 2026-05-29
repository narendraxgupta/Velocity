"""
Velocity — Conan 2.x recipe

Declares the third-party C++ dependencies used by every service.

Workflow
--------

    conan profile detect --force
    conan install . --build=missing -s build_type=Release \
                                    -s compiler.cppstd=20

This generates a CMakePresets.json and a `conan_toolchain.cmake`. Build:

    cmake --preset conan-release
    cmake --build --preset conan-release --parallel

Versions are pinned. Bump them deliberately, never blindly.
"""

from conan import ConanFile
from conan.tools.cmake import CMakeDeps, CMakeToolchain, cmake_layout


class VelocityRecipe(ConanFile):
    name        = "velocity"
    version     = "0.1.0"
    license     = "MIT"
    description = "Distributed benchmarking platform for trading infrastructure"
    settings    = "os", "compiler", "build_type", "arch"

    options = {
        "shared": [True, False],
        "with_fix": [True, False],     # QuickFIX is optional during early dev
    }
    default_options = {
        "shared": False,
        "with_fix": False,
        "boost/*:header_only": False,
        "boost/*:without_python": True,
        "boost/*:without_log": False,
        "openssl/*:no_async": True,
    }

    # ------------------------------------------------------------------
    # Dependencies — version-locked.
    # ------------------------------------------------------------------
    def requirements(self):
        # Build / RPC
        self.requires("protobuf/5.27.0")
        self.requires("grpc/1.65.0")
        self.requires("abseil/20240722.0", override=True)
        # drogon's trantor pins c-ares/1.25.0 while grpc resolves it via a range
        # to 1.34.6 -> conflict. 1.25.0 is inside grpc's [>=1.19.1 <2] range, so
        # pin there to satisfy both.
        self.requires("c-ares/1.25.0", override=True)

        # Logging & string formatting
        self.requires("spdlog/1.14.1")
        self.requires("fmt/11.0.2", override=True)

        # Concurrency / data structures (Boost.Intrusive for the orderbook;
        # Boost.Asio used by Drogon and friends; Boost.LockFree for SPSC).
        self.requires("boost/1.86.0")

        # Networking — only the API gateway uses Drogon
        self.requires("drogon/1.9.6")

        # WebSockets (uWebSockets) — used by leaderboard-ws and bot-worker
        self.requires("usockets/0.8.6")
        self.requires("uwebsockets/20.66.0")

        # libcurl — portable REST transport in the bot worker
        self.requires("libcurl/8.10.1")

        # io_uring wrapper
        self.requires("liburing/2.7")

        # Kafka client for ingester / validator
        self.requires("librdkafka/2.5.3")

        # Redis client
        self.requires("hiredis/1.2.0")
        self.requires("redis-plus-plus/1.3.13")

        # JSON for HTTP responses
        self.requires("nlohmann_json/3.11.3")

        # Histograms
        self.requires("hdrhistogram-c/0.11.8")

        # Metrics / tracing
        self.requires("prometheus-cpp/1.2.4")
        self.requires("opentelemetry-cpp/1.16.1")

        # FIX engine (optional)
        if self.options.with_fix:
            self.requires("quickfix/1.15.1")

    def build_requirements(self):
        self.tool_requires("cmake/3.30.0")
        self.tool_requires("ninja/1.12.1")
        self.tool_requires("protobuf/5.27.0")
        self.tool_requires("grpc/1.65.0")
        # GoogleTest is a build-time dep — only needed when VELOCITY_BUILD_TESTS
        # is ON. Declared as test_requires so it's pulled in for tests builds
        # but not propagated to library consumers.
        self.test_requires("gtest/1.15.0")

    def layout(self):
        cmake_layout(self)

    def generate(self):
        tc = CMakeToolchain(self)
        tc.cache_variables["VELOCITY_USE_LTO"] = "ON"
        tc.cache_variables["VELOCITY_BUILD_TESTS"] = "ON"
        tc.generate()

        deps = CMakeDeps(self)
        deps.generate()

    def configure(self):
        if self.settings.compiler in ("gcc", "clang"):
            self.settings.compiler.cppstd = "20"
            self.settings.compiler.libcxx = "libstdc++11"
