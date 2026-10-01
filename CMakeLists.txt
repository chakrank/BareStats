cmake_minimum_required(VERSION 3.20)
project(BareStats LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (MSVC)
    add_compile_options(/W4 /utf-8 /permissive- /Zc:__cplusplus)
endif()

set(SOURCES
    src/main.cpp
    src/metrics/cpu_collector.cpp
    src/metrics/memory_collector.cpp
    src/metrics/nvml_collector.cpp
    src/metrics/gpu_collector.cpp
    src/metrics/telemetry_engine.cpp
    src/overlay/overlay_window.cpp
    src/overlay/monitor_helper.cpp
    src/config/config.cpp
    src/util/formatter.cpp
)

set(HEADERS
    src/metrics/metrics.h
    src/metrics/cpu_collector.h
    src/metrics/memory_collector.h
    src/metrics/nvml_collector.h
    src/metrics/gpu_collector.h
    src/metrics/telemetry_engine.h
    src/overlay/overlay_window.h
    src/overlay/monitor_helper.h
    src/config/config.h
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
)

# Copy config.example.ini to build output
add_custom_command(TARGET BareStats POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/config.example.ini"
    "$<TARGET_FILE_DIR:BareStats>/config.example.ini"
)
