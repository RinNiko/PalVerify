cmake_minimum_required(VERSION 3.25)

project(PalVerify VERSION 1.0.0 LANGUAGES CXX)

option(PALVERIFY_BUILD_TESTS "Build PalVerify unit tests" ON)
option(PALVERIFY_BUILD_UE4SS "Build UE4SS runtime adapters" OFF)
set(
    PALVERIFY_UE4SS_ROOT
    ""
    CACHE PATH
    "Path to a Palworld-compatible RE-UE4SS source checkout"
)

add_library(palverify_core STATIC
    src/mod_policy.cpp
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
    src/launcher_state.cpp
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
    add_library(palverify_payload_archive STATIC
        src/payload_archive.cpp
    )
    target_include_directories(
        palverify_payload_archive
        PUBLIC
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    target_link_libraries(
        palverify_payload_archive
        PUBLIC
            cabinet
    )
    target_compile_features(
        palverify_payload_archive
        PUBLIC
            cxx_std_20
    )
    target_compile_options(
        palverify_payload_archive
        PRIVATE
            /W4
            /WX
            /permissive-
            /utf-8
    )
    target_link_libraries(
        palverify_installer_core
        PUBLIC
            palverify_payload_archive
    )

    add_library(palverify_windows_integrity STATIC
        src/client_report.cpp
        src/windows_process_scan.cpp
    )
    target_link_libraries(
        palverify_windows_integrity
        PUBLIC
            palverify_core
            bcrypt
            crypt32
            version
            winhttp
            wintrust
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
        apps/client_agent/client_ui_windows.cpp
    )
    target_include_directories(
        PalVerifyClient
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/apps/client_agent"
    )
    target_link_libraries(
        PalVerifyClient
        PRIVATE
            palverify_windows_integrity
            comctl32
            shell32
    )
    target_compile_features(PalVerifyClient PRIVATE cxx_std_20)
    target_link_options(PalVerifyClient PRIVATE /Brepro)
    target_compile_options(
        PalVerifyClient
        PRIVATE
            /W4
            /WX
            /permissive-
            /utf-8
    )

    add_executable(palverify_client_report_tests
        tests/client_report_tests.cpp
    )
    target_link_libraries(
        palverify_client_report_tests
        PRIVATE
            palverify_windows_integrity
            bcrypt
    )
    target_compile_features(
        palverify_client_report_tests
        PRIVATE
            cxx_std_20
    )
    target_compile_options(
        palverify_client_report_tests
        PRIVATE
            /W4
            /WX
            /permissive-
            /utf-8
    )
    add_test(
        NAME palverify_client_report_tests
        COMMAND palverify_client_report_tests
    )

    add_executable(palverify_windows_process_scan_tests
        tests/windows_process_scan_tests.cpp
        tests/process_identity_fixture.rc
    )
    target_link_libraries(
        palverify_windows_process_scan_tests
        PRIVATE
            palverify_windows_integrity
    )
    target_compile_features(
        palverify_windows_process_scan_tests
        PRIVATE
            cxx_std_20
    )
    target_compile_options(
        palverify_windows_process_scan_tests
        PRIVATE
            /W4
            /WX
            /permissive-
            /utf-8
    )
    add_test(
        NAME palverify_windows_process_scan_tests
        COMMAND palverify_windows_process_scan_tests
    )

    add_executable(palverify_payload_packer
        apps/payload_packer/main.cpp
    )
    target_link_libraries(
        palverify_payload_packer
        PRIVATE
            palverify_payload_archive
    )
    target_compile_features(
        palverify_payload_packer
        PRIVATE
            cxx_std_20
    )
    target_compile_options(
        palverify_payload_packer
        PRIVATE
            /W4
            /WX
            /permissive-
            /utf-8
    )

    set(
        PALVERIFY_GENERATED_DIRECTORY
        "${CMAKE_BINARY_DIR}/generated"
    )
    set(
        PALVERIFY_EMBEDDED_PAYLOAD
        "${PALVERIFY_GENERATED_DIRECTORY}/embedded-palverify-payload.bin"
    )
    set(
        PALVERIFY_EMBEDDED_PAYLOAD_RC
        "${PALVERIFY_GENERATED_DIRECTORY}/embedded-palverify-payload.rc"
    )
    add_custom_command(
        OUTPUT
            "${PALVERIFY_EMBEDDED_PAYLOAD}"
            "${PALVERIFY_EMBEDDED_PAYLOAD_RC}"
        COMMAND
            "${CMAKE_COMMAND}" -E make_directory
            "${PALVERIFY_GENERATED_DIRECTORY}"
        COMMAND
            "$<TARGET_FILE:palverify_payload_packer>"
            "${PALVERIFY_EMBEDDED_PAYLOAD}"
            "${PALVERIFY_EMBEDDED_PAYLOAD_RC}"
            "Info.json"
            "${CMAKE_CURRENT_SOURCE_DIR}/packaging/Info.json"
            "thumbnail.png"
            "${CMAKE_CURRENT_SOURCE_DIR}/packaging/thumbnail.png"
            "client/README.md"
            "${CMAKE_CURRENT_SOURCE_DIR}/packaging/client/README.md"
            "client/enabled.txt"
            "${CMAKE_CURRENT_SOURCE_DIR}/packaging/client/enabled.txt"
            "client/Scripts/config.json"
            "${CMAKE_CURRENT_SOURCE_DIR}/packaging/client/Scripts/config.json"
            "client/Scripts/main.lua"
            "${CMAKE_CURRENT_SOURCE_DIR}/packaging/client/Scripts/main.lua"
            "client/Scripts/PalVerifyClient.exe"
            "$<TARGET_FILE:PalVerifyClient>"
        DEPENDS
            palverify_payload_packer
            PalVerifyClient
            packaging/Info.json
            packaging/thumbnail.png
            packaging/client/README.md
            packaging/client/enabled.txt
            packaging/client/Scripts/config.json
            packaging/client/Scripts/main.lua
        COMMENT
            "Compressing PalVerify payload into Pal3Mien PE resource"
        VERBATIM
    )
    set_source_files_properties(
        "${PALVERIFY_EMBEDDED_PAYLOAD_RC}"
        PROPERTIES
            GENERATED TRUE
    )

    add_executable(Pal3Mien WIN32
        apps/launcher/main.cpp
        resources/launcher/launcher.rc
        "${PALVERIFY_EMBEDDED_PAYLOAD_RC}"
    )
    target_include_directories(
        Pal3Mien
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/launcher"
    )
    target_link_libraries(
        Pal3Mien
        PRIVATE
            palverify_installer_core
            advapi32
            bcrypt
            dwmapi
            gdiplus
            ole32
            shell32
            winhttp
    )
    target_compile_features(Pal3Mien PRIVATE cxx_std_20)
    target_link_options(Pal3Mien PRIVATE /Brepro)
    target_compile_options(
        Pal3Mien
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

    add_executable(palverify_launcher_state_tests
        tests/launcher_state_tests.cpp
    )
    target_link_libraries(
        palverify_launcher_state_tests
        PRIVATE
            palverify_installer_core
    )
    target_compile_features(
        palverify_launcher_state_tests
        PRIVATE
            cxx_std_20
    )
    if(MSVC)
        target_compile_options(
            palverify_launcher_state_tests
            PRIVATE
                /W4
                /WX
                /permissive-
                /utf-8
        )
    endif()
    add_test(
        NAME palverify_launcher_state_tests
        COMMAND palverify_launcher_state_tests
    )
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
