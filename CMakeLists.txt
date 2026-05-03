cmake_minimum_required(VERSION 3.24)

project(pi_lite LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(CURL REQUIRED)
find_package(nlohmann_json QUIET)

if(NOT nlohmann_json_FOUND)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(NLOHMANN_JSON REQUIRED IMPORTED_TARGET nlohmann_json)
endif()

add_executable(pi-lite
  src/agent.cpp
  src/config.cpp
  src/line_reader.cpp
  src/llm_client.cpp
  src/main.cpp
  src/markdown_renderer.cpp
  src/status_indicator.cpp
  src/tool.cpp
  src/tools.cpp
  src/util.cpp
)

target_include_directories(pi-lite PRIVATE src)
target_link_libraries(pi-lite PRIVATE CURL::libcurl)

if(nlohmann_json_FOUND)
  target_link_libraries(pi-lite PRIVATE nlohmann_json::nlohmann_json)
else()
  target_link_libraries(pi-lite PRIVATE PkgConfig::NLOHMANN_JSON)
endif()

if(MSVC)
  target_compile_options(pi-lite PRIVATE /W4)
else()
  target_compile_options(pi-lite PRIVATE -Wall -Wextra -Wpedantic)
endif()
