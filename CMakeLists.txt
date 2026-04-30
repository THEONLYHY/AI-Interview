cmake_minimum_required(VERSION 3.16)

project(ai_interview LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)

add_executable(ai_interview
    src/main.cc
)

target_link_libraries(ai_interview PRIVATE
    nlohmann_json::nlohmann_json
    spdlog::spdlog
)