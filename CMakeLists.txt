# vcpkg 传递依赖（如 utf8cpp）可能使用 $<COMPILE_ONLY:...>，需 CMake 3.27+
cmake_minimum_required(VERSION 3.27)

project(ai_interview LANGUAGES CXX)

# C++ 标准配置
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 导出编译数据库，供 IntelliSense / clangd 使用
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()


option(AI_INTERVIEW_BUILD_TESTS "Build protocol unit tests with local third_party/googletest" ON)
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

# Qt依赖（必需，vcpkg的qtbase是Qt6）
# find_package(Qt6 COMPONENTS Core Widgets Gui QUIET)
# if(Qt6_FOUND)
#     set(QT_VERSION_MAJOR 6)
#     message(STATUS "Found Qt6")
# else()
#     # 尝试Qt5
#     find_package(Qt5 COMPONENTS Core Widgets Gui QUIET)
#     if(Qt5_FOUND)
#         set(QT_VERSION_MAJOR 5)
#         message(STATUS "Found Qt5")
#     else()
#         message(FATAL_ERROR "Qt not found! Please install Qt via vcpkg: vcpkg install qtbase")
#     endif()
# endif()

set(PROJECT_INCLUDE_DIR
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

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

set(MAIN_SOURCES
    #main/test.cc
    # main/dialog_session_realtime_demo_main.cc
    #main/interview_text_pipeline_main.cc
    main/interview_voice_cli_main.cc
)

add_executable(ai_interview
    ${MAIN_SOURCES}
    ${COMMON_SOURCES}
    ${INTERVIEW_SOURCES}
    ${SERVICES_SOURCES}
)

#头文件目录，把include/加入头文件搜索路径
target_include_directories(ai_interview
    PRIVATE
        ${PROJECT_INCLUDE_DIR}
)

target_link_libraries(ai_interview 
    PRIVATE
        # OpenSSL
        OpenSSL::SSL
        OpenSSL::Crypto
        # Boost
        Boost::system
        Boost::thread
        # JSON
        nlohmann_json::nlohmann_json
        # 压缩
        ZLIB::ZLIB
        # 音频
        ${AI_INTERVIEW_PORTAUDIO_TARGET}
        # HTTP客户端
        CURL::libcurl
        # PDF解析
        podofo::podofo
        # 日志
        spdlog::spdlog
        # # Qt界面
        # Qt${QT_VERSION_MAJOR}::Core
        # Qt${QT_VERSION_MAJOR}::Widgets
        # Qt${QT_VERSION_MAJOR}::Gui
)

# # 阶段 5：Mock 实时层脚本 + DialogSession 事件驱动（不依赖真 WSS）
# add_executable(dialog_session_realtime_demo
#     main/dialog_session_realtime_demo_main.cc
#     ${COMMON_SOURCES}
#     ${INTERVIEW_SOURCES}
#     ${SERVICES_SOURCES}
# )

# target_include_directories(dialog_session_realtime_demo
#     PRIVATE
#         ${PROJECT_INCLUDE_DIR}
# )

# target_link_libraries(dialog_session_realtime_demo
#     PRIVATE
#         nlohmann_json::nlohmann_json
#         spdlog::spdlog
#         Boost::system
#         CURL::libcurl
#         ZLIB::ZLIB
#         podofo::podofo
# )

#警告选项
if(MSVC)
    target_compile_options(ai_interview PRIVATE /W4)
    #target_compile_options(dialog_session_realtime_demo PRIVATE /W4)
else()
    target_compile_options(ai_interview PRIVATE -Wall -Wextra -Wpedantic)
    #target_compile_options(dialog_session_realtime_demo PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(AI_INTERVIEW_BUILD_TESTS)
    enable_testing()

    add_executable(dialog_session_regression_test
        tests/dialog_session_regression_test.cc
        src/common/logger.cc
        src/common/config.cc
        src/common/protocol.cc
        src/interview/dialog_session.cc
        src/interview/interview_session.cc
        src/services/mock_llm_client.cc
        src/services/audio_manager.cc
    )
    target_include_directories(dialog_session_regression_test PRIVATE ${PROJECT_INCLUDE_DIR})
    target_link_libraries(dialog_session_regression_test PRIVATE
        nlohmann_json::nlohmann_json
        spdlog::spdlog
        ZLIB::ZLIB
        ${AI_INTERVIEW_PORTAUDIO_TARGET}
    )
    if(MSVC)
        target_compile_options(dialog_session_regression_test PRIVATE /W4)
    else()
        target_compile_options(dialog_session_regression_test PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME dialog_session_regression_test
             COMMAND dialog_session_regression_test)
endif()

# if(AI_INTERVIEW_BUILD_TESTS)
#     set(GOOGLETEST_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/googletest")
#     if(NOT EXISTS "${GOOGLETEST_ROOT}/CMakeLists.txt")
#         message(FATAL_ERROR
#             "未找到 GoogleTest 源码 (${GOOGLETEST_ROOT})。请先 clone：\n"
#             "  git clone --depth 1 --branch v1.17.0 "
#             "https://github.com/google/googletest.git third_party/googletest\n"
#             "或关闭测试：cmake ... -DAI_INTERVIEW_BUILD_TESTS=OFF")
#     endif()

#     set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
#     add_subdirectory("${GOOGLETEST_ROOT}"
#         "${CMAKE_BINARY_DIR}/third_party/googletest-build"
#         EXCLUDE_FROM_ALL)

#     enable_testing()
#     include(GoogleTest)

#     add_executable(ai_interview_tests
#         tests/config_test.cc
#         tests/dialog_session_test.cc
#         tests/interview_session_test.cc
#         tests/protocol_smoke_test.cc
#         src/common/config.cc
#         src/common/logger.cc
#         src/common/protocol.cc
#         src/interview/dialog_session.cc
#         src/interview/interview_session.cc
#         src/services/mock_llm_client.cc
#         src/services/mock_realtime_client.cc
#     )
#     target_include_directories(ai_interview_tests PRIVATE ${PROJECT_INCLUDE_DIR})
#     target_link_libraries(ai_interview_tests PRIVATE
#         GTest::gtest_main
#         nlohmann_json::nlohmann_json
#         spdlog::spdlog
#         ZLIB::ZLIB
#     )
#     if(MSVC)
#         target_compile_options(ai_interview_tests PRIVATE /W4)
#     else()
#         target_compile_options(ai_interview_tests PRIVATE -Wall -Wextra -Wpedantic)
#     endif()

#     gtest_discover_tests(ai_interview_tests)
# endif()
