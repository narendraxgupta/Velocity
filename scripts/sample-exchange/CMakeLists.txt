# =============================================================================
#  velocity-sample-exchange
#
#  A minimal, reference-quality matching engine used both as:
#    * A regression target for the bot fleet (we know exactly what fills
#      should be produced for any input)
#    * A baseline against which real submissions are compared
#
#  This is intentionally simple — it's the "if you can beat *this*" bar.
# =============================================================================

add_executable(velocity-sample-exchange
    src/main.cpp
    src/orderbook.cpp
    src/http_server.cpp
)

target_include_directories(velocity-sample-exchange
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(velocity-sample-exchange
    PRIVATE
        velocity::common
        velocity::compile_options
        Drogon::Drogon
        spdlog::spdlog
        nlohmann_json::nlohmann_json
        Boost::headers
)

install(TARGETS velocity-sample-exchange RUNTIME DESTINATION bin)
