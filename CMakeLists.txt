cmake_minimum_required(VERSION 3.16)

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

find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(Boost REQUIRED COMPONENTS system)
find_package(CURL REQUIRED)

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
    src/services/realtime_client.cc
)

set(MAIN_SOURCES
    main/dialog_session_realtime_demo_main.cc
)

# add_executable(protocol_demo
#     main/protocol_demo_main.cc
#     ${COMMON_SOURCES}
# )

# target_include_directories(protocol_demo
#     PRIVATE
#         ${PROJECT_INCLUDE_DIR}
# )

# target_link_libraries(protocol_demo
#     PRIVATE
#         nlohmann_json::nlohmann_json
#         spdlog::spdlog
#         Boost::system
#         CURL::libcurl
# )

# add_executable(interview_demo
#     main/interview_demo_main.cc
#     ${COMMON_SOURCES}
#     ${INTERVIEW_SOURCES}
#     ${SERVICES_SOURCES}
# )

# target_include_directories(interview_demo
#     PRIVATE
#         ${PROJECT_INCLUDE_DIR}
# )

# target_link_libraries(interview_demo
#     PRIVATE
#         nlohmann_json::nlohmann_json
#         spdlog::spdlog
#         Boost::system
#         CURL::libcurl
# )




# if(MSVC)
#     target_compile_options(protocol_demo PRIVATE /W4)
#     target_compile_options(interview_demo PRIVATE /W4)
# else()
#     target_compile_options(protocol_demo PRIVATE -Wall -Wextra -Wpedantic)
#     target_compile_options(interview_demo PRIVATE -Wall -Wextra -Wpedantic)
# endif()

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
        nlohmann_json::nlohmann_json
        spdlog::spdlog
        Boost::system
        CURL::libcurl
)


#警告选项
if(MSVC)
    target_compile_options(ai_interview PRIVATE /W4)
else()
    target_compile_options(ai_interview PRIVATE -Wall -Wextra -Wpedantic)
endif()