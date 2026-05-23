# vcpkg 传递依赖（如 utf8cpp）可能使用 $<COMPILE_ONLY:...>，需 CMake 3.27+
cmake_minimum_required(VERSION 3.27)

project(ai_interview LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_AUTOMOC ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

option(AI_INTERVIEW_BUILD_TESTS "Build local regression tests" ON)
option(AI_INTERVIEW_USE_SYSTEM_PORTAUDIO
    "Use the system PortAudio package instead of the vcpkg PortAudio target"
    OFF)

find_package(OpenSSL REQUIRED)
find_package(Boost REQUIRED COMPONENTS system thread)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(ZLIB REQUIRED)
find_package(CURL REQUIRED)
find_package(podofo CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(Qt6 COMPONENTS Core Widgets Gui Test REQUIRED)
set(AI_INTERVIEW_QT_OFFSCREEN_PLUGIN_CONFIG
    "${Qt6Gui_DIR}/Qt6QOffscreenIntegrationPluginConfig.cmake")
if(NOT TARGET Qt6::QOffscreenIntegrationPlugin AND
   EXISTS "${AI_INTERVIEW_QT_OFFSCREEN_PLUGIN_CONFIG}")
    include("${AI_INTERVIEW_QT_OFFSCREEN_PLUGIN_CONFIG}")
endif()

if(AI_INTERVIEW_USE_SYSTEM_PORTAUDIO)
    find_package(PkgConfig REQUIRED)
    set(PKG_CONFIG_USE_CMAKE_PREFIX_PATH OFF)
    pkg_check_modules(SYSTEM_PORTAUDIO REQUIRED IMPORTED_TARGET portaudio-2.0)
    set(AI_INTERVIEW_PORTAUDIO_TARGET PkgConfig::SYSTEM_PORTAUDIO)
    message(STATUS "Using system PortAudio via pkg-config")
else()
    find_package(portaudio CONFIG REQUIRED)
    set(AI_INTERVIEW_PORTAUDIO_TARGET portaudio_static)
    message(STATUS "Using vcpkg PortAudio target")
endif()

set(PROJECT_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)

set(COMMON_SOURCES
    src/common/logger.cc
    src/common/config.cc
    src/common/protocol.cc
)

set(INTERVIEW_SOURCES
    src/interview/interview_session.cc
    src/interview/dialog_session.cc
)

set(SERVICES_SOURCES
    src/services/mock_llm_client.cc
    src/services/real_llm_client.cc
    src/services/mock_realtime_client.cc
    src/services/real_realtime_client.cc
    src/services/pdf_parser.cc
    src/services/audio_manager.cc
)

function(ai_interview_apply_warnings target_name)
    if(MSVC)
        target_compile_options(${target_name} PRIVATE /W4)
    else()
        target_compile_options(${target_name} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

add_library(ai_interview_core STATIC
    ${COMMON_SOURCES}
    ${INTERVIEW_SOURCES}
    ${SERVICES_SOURCES}
)

target_include_directories(ai_interview_core
    PUBLIC
        ${PROJECT_INCLUDE_DIR}
)

target_link_libraries(ai_interview_core
    PUBLIC
        OpenSSL::SSL
        OpenSSL::Crypto
        Boost::system
        Boost::thread
        nlohmann_json::nlohmann_json
        ZLIB::ZLIB
        ${AI_INTERVIEW_PORTAUDIO_TARGET}
        CURL::libcurl
        podofo::podofo
        spdlog::spdlog
)

ai_interview_apply_warnings(ai_interview_core)

add_executable(ai_interview_text_pipeline
    main/interview_text_pipeline_main.cc
)
target_link_libraries(ai_interview_text_pipeline PRIVATE ai_interview_core)
ai_interview_apply_warnings(ai_interview_text_pipeline)

add_executable(ai_interview_voice_cli
    main/interview_voice_cli_main.cc
)
target_link_libraries(ai_interview_voice_cli PRIVATE ai_interview_core)
ai_interview_apply_warnings(ai_interview_voice_cli)

# Backward-compatible default binary; new scripts/docs should prefer ai_interview_voice_cli.
add_executable(ai_interview
    main/interview_voice_cli_main.cc
)
target_link_libraries(ai_interview PRIVATE ai_interview_core)
ai_interview_apply_warnings(ai_interview)

add_executable(ai_interview_qt
    main/main_qt.cc
    include/ui/config_dialog.h
    include/ui/mainwindow.h
    src/ui/config_dialog.cc
    src/ui/mainwindow.cc
)
target_link_libraries(ai_interview_qt
    PRIVATE
        ai_interview_core
        Qt6::Core
        Qt6::Widgets
        Qt6::Gui
)
ai_interview_apply_warnings(ai_interview_qt)

if(AI_INTERVIEW_BUILD_TESTS)
    enable_testing()

    function(ai_interview_add_test test_name source_file)
        add_executable(${test_name} ${source_file})
        target_link_libraries(${test_name} PRIVATE ai_interview_core)
        ai_interview_apply_warnings(${test_name})
        add_test(NAME ${test_name} COMMAND ${test_name})
    endfunction()

    function(ai_interview_add_qt_test test_name source_file)
        add_executable(${test_name}
            ${source_file}
            include/ui/config_dialog.h
            include/ui/mainwindow.h
            src/ui/config_dialog.cc
            src/ui/mainwindow.cc
        )
        target_link_libraries(${test_name}
            PRIVATE
                ai_interview_core
                Qt6::Core
                Qt6::Widgets
                Qt6::Gui
                Qt6::Test
        )
        if(TARGET Qt6::QOffscreenIntegrationPlugin)
            qt_import_plugins(${test_name}
                INCLUDE Qt6::QOffscreenIntegrationPlugin)
        endif()
        ai_interview_apply_warnings(${test_name})
        add_test(NAME ${test_name} COMMAND ${test_name})
        set_tests_properties(${test_name}
            PROPERTIES ENVIRONMENT
                "QT_QPA_PLATFORM=offscreen;XDG_CACHE_HOME=/tmp")
    endfunction()

    ai_interview_add_test(protocol_smoke_test tests/protocol_smoke_test.cc)
    ai_interview_add_test(interview_session_test tests/interview_session_test.cc)
    ai_interview_add_test(dialog_session_regression_test
                          tests/dialog_session_regression_test.cc)
    ai_interview_add_test(audio_manager_test tests/audio_manager_test.cc)
    ai_interview_add_qt_test(qt_ui_smoke_test tests/qt_ui_smoke_test.cc)
endif()
