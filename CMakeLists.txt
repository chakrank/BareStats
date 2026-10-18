cmake_minimum_required(VERSION 3.20)
project(BareStats VERSION 1.3.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (APPLE)
    enable_language(OBJCXX)
    set(CMAKE_OBJCXX_STANDARD 17)
    set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
    add_compile_options("$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>")
    if (NOT CMAKE_OSX_ARCHITECTURES)
        set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Build universal binary for macOS" FORCE)
    endif()
endif()

if (WIN32)
    add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX)

    if (MSVC)
        add_compile_options(/W4 /utf-8 /permissive- /Zc:__cplusplus)
        add_compile_options("$<$<CONFIG:Release>:/O2;/Oi;/Gy>")
        add_link_options("$<$<CONFIG:Release>:/OPT:REF;/OPT:ICF>")
    endif()
else()
    add_compile_options(-Wall -Wextra -pedantic)
    add_compile_options("$<$<CONFIG:Release>:-O3>")
endif()

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/config
    ${CMAKE_CURRENT_SOURCE_DIR}/src/metrics
    ${CMAKE_CURRENT_SOURCE_DIR}/src/overlay
    ${CMAKE_CURRENT_SOURCE_DIR}/src/util
)

set(COMMON_SOURCES
    src/metrics/gpu_collector.cpp
    src/metrics/nvml_collector.cpp
    src/metrics/telemetry_engine.cpp
    src/config/config.cpp
    src/config/config_watcher.cpp
    src/config/ini_parser.cpp
    src/util/formatter.cpp
    src/overlay/monitor_helper.cpp
)

set(COMMON_HEADERS
    src/metrics/metrics.h
    src/metrics/cpu_collector.h
    src/metrics/memory_collector.h
    src/metrics/network_collector.h
    src/metrics/gpu_collector.h
    src/metrics/nvml_collector.h
    src/metrics/telemetry_engine.h
    src/config/config.h
    src/config/config_watcher.h
    src/config/ini_parser.h
    src/util/formatter.h
    src/util/geometry.h
    src/overlay/monitor_helper.h
)

if (WIN32)
    include_directories(
        ${CMAKE_CURRENT_SOURCE_DIR}/src/metrics/windows
        ${CMAKE_CURRENT_SOURCE_DIR}/src/overlay/windows
        ${CMAKE_CURRENT_SOURCE_DIR}/resources/windows
    )

    set(PLATFORM_SOURCES
        src/metrics/windows/cpu_collector.cpp
        src/metrics/windows/memory_collector.cpp
        src/metrics/windows/network_collector.cpp
        src/metrics/windows/adl_collector.cpp
        src/metrics/windows/igcl_collector.cpp
        src/metrics/windows/d3dkmt_collector.cpp
        src/overlay/windows/overlay_window.cpp
        resources/windows/resource.rc
        resources/windows/BareStats.manifest
    )

    set(PLATFORM_HEADERS
        src/metrics/windows/adl_collector.h
        src/metrics/windows/igcl_collector.h
        src/metrics/windows/d3dkmt_collector.h
        src/overlay/windows/overlay_window.h
        resources/windows/resource.h
    )

    add_executable(BareStats WIN32 src/main.cpp ${COMMON_SOURCES} ${PLATFORM_SOURCES} ${COMMON_HEADERS} ${PLATFORM_HEADERS})

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
elseif (APPLE)
    include_directories(
        ${CMAKE_CURRENT_SOURCE_DIR}/src/metrics/macos
        ${CMAKE_CURRENT_SOURCE_DIR}/src/overlay/macos
        ${CMAKE_CURRENT_SOURCE_DIR}/resources/macos
    )

    set(PLATFORM_SOURCES
        src/metrics/macos/cpu_collector.cpp
        src/metrics/macos/memory_collector.cpp
        src/metrics/macos/network_collector.cpp
        src/metrics/macos/macos_gpu_collector.mm
        src/overlay/macos/macos_overlay.mm
    )

    set(PLATFORM_HEADERS
        src/metrics/macos/macos_gpu_collector.h
        src/overlay/macos/macos_overlay.h
    )

    set_source_files_properties(
        src/metrics/macos/macos_gpu_collector.mm
        src/overlay/macos/macos_overlay.mm
        PROPERTIES LANGUAGE OBJCXX
        COMPILE_FLAGS "-fobjc-arc"
    )

    set(MACOSX_BUNDLE_ICON_FILE "barestats.icns")
    set(MACOSX_BUNDLE_BUNDLE_NAME "BareStats")
    set(MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.chakrank.barestats")
    set(MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}")
    set(MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}")
    set(MACOSX_BUNDLE_COPYRIGHT "Copyright © 2024-2026 Chakrank. All rights reserved.")

    set(MACOS_BUNDLE_RESOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/macos/barestats.icns"
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/linux/barestats.png"
    )
    set_source_files_properties(${MACOS_BUNDLE_RESOURCES} PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources"
    )

    add_executable(BareStats MACOSX_BUNDLE
        src/main.cpp
        ${COMMON_SOURCES}
        ${PLATFORM_SOURCES}
        ${COMMON_HEADERS}
        ${PLATFORM_HEADERS}
        ${MACOS_BUNDLE_RESOURCES}
    )

    target_link_libraries(BareStats PRIVATE
        "-framework Cocoa"
        "-framework Metal"
        "-framework IOKit"
        "-framework CoreGraphics"
        "-framework CoreText"
        "-framework CoreFoundation"
        pthread
    )

    set_target_properties(BareStats PROPERTIES
        OUTPUT_NAME "BareStats"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/resources/macos/Info.plist.in"
        MACOSX_BUNDLE_ICON_FILE "barestats.icns"
    )
else()
    include_directories(
        ${CMAKE_CURRENT_SOURCE_DIR}/src/metrics/linux
        ${CMAKE_CURRENT_SOURCE_DIR}/src/overlay/linux
    )

    set(PLATFORM_SOURCES
        src/metrics/linux/cpu_collector.cpp
        src/metrics/linux/memory_collector.cpp
        src/metrics/linux/network_collector.cpp
        src/metrics/linux/linux_proc_collector.cpp
        src/metrics/linux/linux_sysfs_gpu_collector.cpp
        src/overlay/linux/linux_overlay.cpp
    )

    set(PLATFORM_HEADERS
        src/metrics/linux/linux_proc_collector.h
        src/metrics/linux/linux_sysfs_gpu_collector.h
        src/overlay/linux/linux_overlay.h
    )

    find_package(PkgConfig QUIET)
    set(LINUX_EXTRA_LIBS)

    if (PKG_CONFIG_FOUND)
        pkg_check_modules(CAIRO QUIET cairo)
        if (CAIRO_FOUND)
            add_compile_definitions(BARESTATS_HAS_CAIRO)
            list(APPEND LINUX_EXTRA_LIBS ${CAIRO_LIBRARIES})
            include_directories(${CAIRO_INCLUDE_DIRS})
        endif()

        pkg_check_modules(X11 QUIET x11 xfixes)
        if (X11_FOUND)
            add_compile_definitions(BARESTATS_HAS_X11)
            list(APPEND LINUX_EXTRA_LIBS ${X11_LIBRARIES})
            include_directories(${X11_INCLUDE_DIRS})
        endif()

        pkg_check_modules(WAYLAND QUIET wayland-client wayland-cursor)
        if (WAYLAND_FOUND)
            add_compile_definitions(BARESTATS_HAS_WAYLAND)
            list(APPEND LINUX_EXTRA_LIBS ${WAYLAND_LIBRARIES})
            include_directories(${WAYLAND_INCLUDE_DIRS})
        endif()
    endif()

    add_executable(BareStats src/main.cpp ${COMMON_SOURCES} ${PLATFORM_SOURCES} ${COMMON_HEADERS} ${PLATFORM_HEADERS})
    target_link_libraries(BareStats PRIVATE
        pthread
        dl
        ${LINUX_EXTRA_LIBS}
    )
    set_target_properties(BareStats PROPERTIES OUTPUT_NAME "barestats")
endif()

# Copy config.example.ini to build output
add_custom_command(TARGET BareStats POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/config.example.ini"
    "$<TARGET_FILE_DIR:BareStats>/config.example.ini"
)

option(BARESTATS_BUILD_TESTS "Build BareStats automated verification test suite and driver mocks" ON)

if (BARESTATS_BUILD_TESTS)
    enable_testing()

    if (WIN32)
        # Synthetic Driver Mocks for Hardware-Agnostic Validation
        add_library(mock_nvml SHARED tests/mocks/mock_nvml.cpp)
        set_target_properties(mock_nvml PROPERTIES PREFIX "")

        add_library(mock_atiadlxx SHARED tests/mocks/mock_atiadlxx.cpp)
        set_target_properties(mock_atiadlxx PROPERTIES PREFIX "")

        add_library(mock_ControlLib SHARED tests/mocks/mock_ControlLib.cpp)
        set_target_properties(mock_ControlLib PROPERTIES PREFIX "")
    else()
        add_library(mock_nvml SHARED tests/mocks/mock_nvml.cpp)
        set_target_properties(mock_nvml PROPERTIES PREFIX "lib")
    endif()

    set(TEST_SOURCES
        tests/test_main.cpp
        tests/test_config.cpp
        tests/test_formatters.cpp
        tests/test_overlay.cpp
        tests/test_metrics_common.cpp
        ${COMMON_SOURCES}
    )

    if (WIN32)
        list(APPEND TEST_SOURCES
            tests/test_metrics_windows.cpp
            src/metrics/windows/cpu_collector.cpp
            src/metrics/windows/memory_collector.cpp
            src/metrics/windows/network_collector.cpp
            src/metrics/windows/d3dkmt_collector.cpp
            src/metrics/windows/adl_collector.cpp
            src/metrics/windows/igcl_collector.cpp
            src/overlay/windows/overlay_window.cpp
            tests/test_metrics_linux.cpp
            src/metrics/linux/linux_proc_collector.cpp
            src/metrics/linux/linux_sysfs_gpu_collector.cpp
            src/overlay/linux/linux_overlay.cpp
        )
    elseif (APPLE)
        list(APPEND TEST_SOURCES
            tests/test_metrics_macos.cpp
            src/metrics/macos/cpu_collector.cpp
            src/metrics/macos/memory_collector.cpp
            src/metrics/macos/network_collector.cpp
            src/metrics/macos/macos_gpu_collector.mm
            src/overlay/macos/macos_overlay.mm
            tests/test_metrics_linux.cpp
            src/metrics/linux/linux_proc_collector.cpp
            src/metrics/linux/linux_sysfs_gpu_collector.cpp
            src/overlay/linux/linux_overlay.cpp
        )
        set_source_files_properties(
            src/metrics/macos/macos_gpu_collector.mm
            src/overlay/macos/macos_overlay.mm
            PROPERTIES LANGUAGE OBJCXX
            COMPILE_FLAGS "-fobjc-arc"
        )
    else()
        list(APPEND TEST_SOURCES
            tests/test_metrics_linux.cpp
            src/metrics/linux/cpu_collector.cpp
            src/metrics/linux/memory_collector.cpp
            src/metrics/linux/network_collector.cpp
            src/metrics/linux/linux_proc_collector.cpp
            src/metrics/linux/linux_sysfs_gpu_collector.cpp
            src/overlay/linux/linux_overlay.cpp
        )
    endif()

    add_executable(BareStatsTests ${TEST_SOURCES})

    if (WIN32)
        target_link_libraries(BareStatsTests PRIVATE
            dxgi
            gdiplus
            gdi32
            user32
            shell32
            dwmapi
            powrprof
            iphlpapi
            ws2_32
        )
        add_dependencies(BareStatsTests mock_nvml mock_atiadlxx mock_ControlLib)
    elseif (APPLE)
        target_link_libraries(BareStatsTests PRIVATE
            "-framework Cocoa"
            "-framework Metal"
            "-framework IOKit"
            "-framework CoreGraphics"
            "-framework CoreText"
            "-framework CoreFoundation"
            pthread
            dl
        )
        add_dependencies(BareStatsTests mock_nvml)
    else()
        target_link_libraries(BareStatsTests PRIVATE
            pthread
            dl
            ${LINUX_EXTRA_LIBS}
        )
        add_dependencies(BareStatsTests mock_nvml)
    endif()

    add_test(NAME AutomatedVerification COMMAND BareStatsTests)
endif()

# ==============================================================================
# Installation & CPack Multi-Distribution Packaging
# ==============================================================================
include(GNUInstallDirs)

if (APPLE)
    install(TARGETS BareStats
        BUNDLE DESTINATION .
    )
    install(FILES config.example.ini
        DESTINATION BareStats.app/Contents/Resources
    )
    install(FILES LICENSE
        DESTINATION BareStats.app/Contents/Resources
    )
    install(FILES resources/macos/barestats.icns
        DESTINATION BareStats.app/Contents/Resources
    )
elseif (UNIX)
    install(TARGETS BareStats
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )

    install(FILES resources/linux/barestats.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications
    )

    install(FILES resources/linux/barestats.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications
        RENAME io.github.chakrank.barestats.desktop
    )

    install(FILES resources/linux/barestats.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps
    )

    install(FILES resources/linux/barestats.png
        DESTINATION ${CMAKE_INSTALL_DATADIR}/pixmaps
    )

    install(FILES resources/linux/barestats.png
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps
    )

    install(FILES resources/linux/io.github.chakrank.barestats.metainfo.xml
        DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo
    )

    install(FILES config.example.ini
        DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/barestats
    )

    install(FILES LICENSE
        DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/barestats
        RENAME copyright
    )
else()
    install(TARGETS BareStats
        RUNTIME DESTINATION .
    )
    install(FILES config.example.ini
        DESTINATION .
    )
    install(FILES LICENSE
        DESTINATION .
    )
endif()

# macOS Drag-and-Drop Disk Image (.dmg)
if (APPLE)
    set(CPACK_GENERATOR "DragNDrop")
    set(CPACK_DMG_VOLUME_NAME "BareStats")
    set(CPACK_DMG_FORMAT "UDZO")
    set(CPACK_PACKAGE_FILE_NAME "barestats-${PROJECT_VERSION}-macOS")
endif()

set(CPACK_PACKAGE_NAME "barestats")
set(CPACK_PACKAGE_VENDOR "Chakrank")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "BareStats - Ultra-lightweight native telemetry overlay")
set(CPACK_PACKAGE_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(CPACK_PACKAGE_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(CPACK_PACKAGE_VERSION_PATCH ${PROJECT_VERSION_PATCH})
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/chakrank/BareStats")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")

# Debian / Ubuntu packaging (.deb)
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Chakrank <chakrank@hotmail.com>")
set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_DESCRIPTION "BareStats - Ultra-lightweight native telemetry overlay\n BareStats is an unprivileged, ultra-lightweight telemetry overlay delivering\n real-time CPU, GPU, VRAM, RAM, and network throughput at 0.00% idle duty cycle.\n Developed by Chakrank.")
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/debian/postinst;${CMAKE_CURRENT_SOURCE_DIR}/packaging/debian/postrm"
)
set(CPACK_DEBIAN_FILE_NAME "DEB-DEFAULT")

# Fedora / RHEL / openSUSE packaging (.rpm)
set(CPACK_RPM_PACKAGE_SUMMARY "BareStats - Ultra-lightweight native telemetry overlay")
set(CPACK_RPM_PACKAGE_VENDOR "Chakrank")
set(CPACK_RPM_PACKAGE_URL "https://github.com/chakrank/BareStats")
set(CPACK_RPM_PACKAGE_LICENSE "MIT")
set(CPACK_RPM_PACKAGE_GROUP "Applications/System")
set(CPACK_RPM_PACKAGE_AUTOREQPROV ON)
set(CPACK_RPM_PACKAGE_DESCRIPTION "BareStats is an unprivileged, ultra-lightweight telemetry overlay delivering\nreal-time CPU, GPU, VRAM, RAM, and network throughput at 0.00% idle duty cycle.\nEngineered for local AI/ML workloads, competitive gaming, and workstation productivity.\nDeveloped by Chakrank.")
set(CPACK_RPM_FILE_NAME "RPM-DEFAULT")

# Universal portable archives (.tar.gz and .zip)
set(CPACK_ARCHIVE_FILE_NAME "barestats-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")

include(CPack)
