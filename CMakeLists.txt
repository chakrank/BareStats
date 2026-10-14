cmake_minimum_required(VERSION 3.20)
project(BareStats VERSION 1.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX)

if (MSVC)
    add_compile_options(/W4 /utf-8 /permissive- /Zc:__cplusplus)
    add_compile_options("$<$<CONFIG:Release>:/O2;/Oi;/Gy>")
    add_link_options("$<$<CONFIG:Release>:/OPT:REF;/OPT:ICF>")
endif()

set(SOURCES
    src/main.cpp
    src/metrics/cpu_collector.cpp
    src/metrics/memory_collector.cpp
    src/metrics/nvml_collector.cpp
    src/metrics/adl_collector.cpp
    src/metrics/igcl_collector.cpp
    src/metrics/d3dkmt_collector.cpp
    src/metrics/gpu_collector.cpp
    src/metrics/network_collector.cpp
    src/metrics/telemetry_engine.cpp
    src/overlay/overlay_window.cpp
    src/overlay/monitor_helper.cpp
    src/config/config.cpp
    src/config/config_watcher.cpp
    src/util/formatter.cpp
    resources/resource.rc
    resources/BareStats.manifest
)

set(HEADERS
    src/metrics/metrics.h
    src/metrics/cpu_collector.h
    src/metrics/memory_collector.h
    src/metrics/nvml_collector.h
    src/metrics/adl_collector.h
    src/metrics/igcl_collector.h
    src/metrics/d3dkmt_collector.h
    src/metrics/gpu_collector.h
    src/metrics/network_collector.h
    src/metrics/telemetry_engine.h
    src/overlay/overlay_window.h
    src/overlay/monitor_helper.h
    src/config/config.h
    src/config/config_watcher.h
    src/util/formatter.h
)

add_executable(BareStats WIN32 ${SOURCES} ${HEADERS})

target_link_libraries(BareStats PRIVATE
    dxgi
    gdiplus
    user32
    gdi32
    shell32
    dwmapi
    powrprof
    iphlpapi
    ws2_32
)

# Copy config.example.ini to build output
add_custom_command(TARGET BareStats POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/config.example.ini"
    "$<TARGET_FILE_DIR:BareStats>/config.example.ini"
)

option(BARESTATS_BUILD_TESTS "Build BareStats automated verification test suite and driver mocks" ON)

if (BARESTATS_BUILD_TESTS)
    enable_testing()

    # Synthetic Driver Mocks for Hardware-Agnostic Validation
    add_library(mock_nvml SHARED tests/mocks/mock_nvml.cpp)
    set_target_properties(mock_nvml PROPERTIES PREFIX "")

    add_library(mock_atiadlxx SHARED tests/mocks/mock_atiadlxx.cpp)
    set_target_properties(mock_atiadlxx PROPERTIES PREFIX "")

    add_library(mock_ControlLib SHARED tests/mocks/mock_ControlLib.cpp)
    set_target_properties(mock_ControlLib PROPERTIES PREFIX "")

    set(TEST_SOURCES
        tests/test_suite.cpp
        src/metrics/cpu_collector.cpp
        src/metrics/memory_collector.cpp
        src/metrics/network_collector.cpp
        src/metrics/d3dkmt_collector.cpp
        src/metrics/nvml_collector.cpp
        src/metrics/adl_collector.cpp
        src/metrics/igcl_collector.cpp
        src/metrics/gpu_collector.cpp
        src/config/config.cpp
        src/overlay/monitor_helper.cpp
        src/util/formatter.cpp
    )

    add_executable(BareStatsTests ${TEST_SOURCES})

    target_link_libraries(BareStatsTests PRIVATE
        dxgi
        gdi32
        user32
        iphlpapi
        ws2_32
    )

    add_dependencies(BareStatsTests mock_nvml mock_atiadlxx mock_ControlLib)

    add_test(NAME AutomatedVerification COMMAND BareStatsTests)
endif()

