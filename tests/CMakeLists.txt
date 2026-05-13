# Store handler unit tests — Apache 2.0.
include(GoogleTest)

add_executable(test_store test_store.cpp)
target_link_libraries(test_store PRIVATE
    GoodNet::sdk
    goodnet_store_objects
    spdlog::spdlog
    GTest::gtest
    GTest::gtest_main)
target_compile_features(test_store PRIVATE cxx_std_23)
## PCH intentionally skipped — the umbrella header pulls spdlog/asio
## that aren't on this target's include path; the test is short
## enough that a clean compile per TU has no measurable cost.
gtest_discover_tests(test_store
    DISCOVERY_TIMEOUT 30
    PROPERTIES TIMEOUT 60)
