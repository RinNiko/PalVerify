cmake_minimum_required(VERSION 3.25)

project(PalVerify VERSION 0.1.0 LANGUAGES CXX)

option(PALVERIFY_BUILD_TESTS "Build PalVerify unit tests" ON)
option(PALVERIFY_BUILD_UE4SS "Build UE4SS runtime adapters" OFF)
set(
    PALVERIFY_UE4SS_ROOT
    ""
    CACHE PATH
    "Path to a Palworld-compatible RE-UE4SS source checkout"
)

add_library(palverify_core STATIC
    src/platform_policy.cpp
    src/process_rules.cpp
    src/session_guard.cpp
)

target_include_directories(palverify_core
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_features(palverify_core PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(palverify_core PRIVATE /W4 /WX /permissive-)
else()
    target_compile_options(palverify_core PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_library(palverify_installer_core STATIC
    src/installer_settings.cpp
)
target_include_directories(
    palverify_installer_core
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_compile_features(palverify_installer_core PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(
        palverify_installer_core
        PRIVATE
            /W4
            /WX
            /permissive-
    )
else()
    target_compile_options(
        palverify_installer_core
        PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Werror
    )
endif()

if(WIN32)
    add_library(palverify_windows_integrity STATIC
        src/windows_process_scan.cpp
    )
    target_link_libraries(
        palverify_windows_integrity
        PUBLIC
            palverify_core
    )
    target_include_directories(
        palverify_windows_integrity
        PUBLIC
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    target_compile_features(
        palverify_windows_integrity
        PUBLIC
            cxx_std_20
    )

    if(MSVC)
        target_compile_options(
            palverify_windows_integrity
            PRIVATE
                /W4
                /WX
                /permissive-
        )
    endif()

    add_executable(PalVerifyClient
        apps/client_agent/main.cpp
    )
    target_link_libraries(
        PalVerifyClient
        PRIVATE
            palverify_windows_integrity
            shell32
    )
    target_compile_features(PalVerifyClient PRIVATE cxx_std_20)
    target_compile_options(
        PalVerifyClient
        PRIVATE
            /W4
            /WX
            /permissive-
            /utf-8
    )

    add_executable(PalVerifyLauncher WIN32
        apps/installer/main.cpp
    )
    target_link_libraries(
        PalVerifyLauncher
        PRIVATE
            palverify_installer_core
            advapi32
            shell32
    )
    target_compile_features(PalVerifyLauncher PRIVATE cxx_std_20)
    target_compile_options(
        PalVerifyLauncher
        PRIVATE
            /W4
            /WX
            /permissive-
            /utf-8
    )
endif()

if(PALVERIFY_BUILD_TESTS)
    enable_testing()

    add_executable(palverify_core_tests
        tests/core_tests.cpp
    )
    target_link_libraries(palverify_core_tests PRIVATE palverify_core)
    target_compile_features(palverify_core_tests PRIVATE cxx_std_20)

    if(MSVC)
        target_compile_options(palverify_core_tests PRIVATE /W4 /WX /permissive-)
    else()
        target_compile_options(
            palverify_core_tests
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
                -Werror
        )
    endif()

    add_test(NAME palverify_core_tests COMMAND palverify_core_tests)

    add_executable(palverify_installer_tests
        tests/installer_tests.cpp
    )
    target_link_libraries(
        palverify_installer_tests
        PRIVATE
            palverify_installer_core
    )
    target_compile_features(
        palverify_installer_tests
        PRIVATE
            cxx_std_20
    )

    if(MSVC)
        target_compile_options(
            palverify_installer_tests
            PRIVATE
                /W4
                /WX
                /permissive-
        )
    endif()

    add_test(NAME palverify_installer_tests COMMAND palverify_installer_tests)
endif()

if(PALVERIFY_BUILD_UE4SS)
    if(NOT PALVERIFY_UE4SS_ROOT)
        message(
            FATAL_ERROR
            "PALVERIFY_UE4SS_ROOT is required when PALVERIFY_BUILD_UE4SS=ON"
        )
    endif()

    add_subdirectory(
        "${PALVERIFY_UE4SS_ROOT}"
        "${CMAKE_BINARY_DIR}/re-ue4ss"
        EXCLUDE_FROM_ALL
    )
    add_subdirectory(runtime/ue4ss)
endif()
