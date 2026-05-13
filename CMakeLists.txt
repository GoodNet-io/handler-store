cmake_minimum_required(VERSION 3.22)

# Store handler — Apache 2.0. Brings the legacy `apps/store` surface
# forward as a v1 handler plugin: pluggable IStore backend (memory
# reference shipped here; sqlite + DHT + Redis planned), wire
# dispatcher for the seven STORE_* envelope types, exposes the
# `gn.store` extension for in-process callers.
#
# Standalone: `find_package(GoodNet REQUIRED)` brings the SDK +
# `add_plugin()` helper from the installed kernel package.

if(NOT TARGET GoodNet::sdk)
    project(goodnet_handler_store
        VERSION   1.0.0
        LANGUAGES CXX)
    find_package(GoodNet REQUIRED)
    include(CTest)
    if(BUILD_TESTING)
        find_package(GTest      REQUIRED)
        find_package(rapidcheck QUIET)
        goodnet_patch_rapidcheck_targets()
        set(GOODNET_BUILD_TESTS ON)
    endif()
endif()

add_library(goodnet_store_objects OBJECT
    store.cpp
)
target_link_libraries(goodnet_store_objects PUBLIC GoodNet::sdk)
target_compile_features(goodnet_store_objects PUBLIC cxx_std_23)
target_compile_options(goodnet_store_objects PRIVATE
    -Wall -Wextra -Wpedantic -Wno-unused-parameter)
target_include_directories(goodnet_store_objects PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(goodnet_store_objects PROPERTIES
    POSITION_INDEPENDENT_CODE ON)

add_plugin(goodnet_handler_store
    plugin_entry.cpp
)
target_link_libraries(goodnet_handler_store
    PRIVATE goodnet_store_objects)

if(GOODNET_BUILD_TESTS)
    add_subdirectory(tests)
endif()
