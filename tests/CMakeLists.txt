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

if(GOODNET_STORE_WITH_SQLITE)
    add_executable(test_sqlite_backend test_sqlite_backend.cpp)
    target_link_libraries(test_sqlite_backend PRIVATE
        GoodNet::sdk
        goodnet_store_objects
        spdlog::spdlog
        GTest::gtest
        GTest::gtest_main)
    target_compile_features(test_sqlite_backend PRIVATE cxx_std_23)
    gtest_discover_tests(test_sqlite_backend
        DISCOVERY_TIMEOUT 30
        PROPERTIES TIMEOUT 60)
endif()
