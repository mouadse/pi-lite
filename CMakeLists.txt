cmake_minimum_required(VERSION 3.24)

project(pi_lite LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(CURL REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(nlohmann_json QUIET)

if(NOT nlohmann_json_FOUND)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(NLOHMANN_JSON REQUIRED IMPORTED_TARGET nlohmann_json)
endif()

set(PI_LITE_COMMON_SOURCES
  src/agent.cpp
  src/config.cpp
  src/line_reader.cpp
  src/llm_client.cpp
  src/memory.cpp
  src/markdown_renderer.cpp
  src/status_indicator.cpp
  src/tool.cpp
  src/tools.cpp
  src/util.cpp
)

add_executable(pi-lite
  src/main.cpp
  ${PI_LITE_COMMON_SOURCES}
)

add_executable(pi-lite-memory-bench
  benchmarks/memory_ops_bench.cpp
  src/config.cpp
  src/llm_client.cpp
  src/memory.cpp
  src/tool.cpp
  src/util.cpp
)

foreach(target pi-lite pi-lite-memory-bench)
  target_include_directories(${target} PRIVATE src)
  target_link_libraries(${target} PRIVATE CURL::libcurl SQLite3::SQLite3)

  if(nlohmann_json_FOUND)
    target_link_libraries(${target} PRIVATE nlohmann_json::nlohmann_json)
  else()
    target_link_libraries(${target} PRIVATE PkgConfig::NLOHMANN_JSON)
  endif()

  if(MSVC)
    target_compile_options(${target} PRIVATE /W4)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -ffunction-sections -fdata-sections)
    target_link_options(${target} PRIVATE -Wl,--gc-sections)
  endif()
endforeach()
