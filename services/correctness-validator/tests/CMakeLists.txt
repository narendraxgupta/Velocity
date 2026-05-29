add_executable(velocity_validator_tests
    reference_orderbook_test.cpp
    microstructure_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/reference_orderbook.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/microstructure.cpp
)

target_link_libraries(velocity_validator_tests
    PRIVATE
        velocity::compile_options
        velocity::common
        Boost::headers
        GTest::gtest
        GTest::gtest_main
)

target_include_directories(velocity_validator_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

gtest_discover_tests(velocity_validator_tests)
