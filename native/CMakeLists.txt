cmake_minimum_required(VERSION 3.16)

# Minimal build for the Tracy MCP query backend. It links Tracy's analysis
# library (TracyServer / Worker) WITHOUT the GUI/network deps that the full
# vendor.cmake fetches (ImGui, glfw, curl, freetype, usearch, tidy, ...).
# Only capstone + zstd + PPQSort are needed by TracyServer.

option(NO_ISA_EXTENSIONS "Disable -march=native / AVX2" OFF)
set(NO_STATISTICS OFF)  # MUST stay OFF: this is what builds SourceLocationZones stats

# Path to the Tracy source checkout (server/, public/, cmake/ live here).
set(TRACY_DIR "D:/ajin/opensourceProjs/tracy_0.13.1" CACHE PATH "Tracy source root")

include(${TRACY_DIR}/cmake/version.cmake)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

project(tracy-mcp-query LANGUAGES C CXX VERSION ${TRACY_VERSION_STRING})

# Cache CPM downloads next to this project so reconfigure is fast/offline.
set(CPM_SOURCE_CACHE "${CMAKE_CURRENT_LIST_DIR}/.cpm_cache" CACHE PATH "CPM cache")
include(${TRACY_DIR}/cmake/CPM.cmake)

# --- Only the three deps TracyServer actually needs -------------------------

# capstone (Worker.cpp includes <capstone.h>); x86 only to keep it small.
CPMAddPackage(
    NAME capstone
    GITHUB_REPOSITORY capstone-engine/capstone
    GIT_TAG 6.0.0-Alpha5
    OPTIONS
        "CAPSTONE_ARCHITECTURE_DEFAULT OFF"
        "CAPSTONE_X86_SUPPORT ON"
        "CAPSTONE_X86_ATT_DISABLE ON"
        "CAPSTONE_BUILD_CSTOOL OFF"
        "CAPSTONE_BUILD_MACOS_THIN ON"
    EXCLUDE_FROM_ALL TRUE
)
add_library(TracyCapstone INTERFACE)
target_include_directories(TracyCapstone INTERFACE ${capstone_SOURCE_DIR}/include/capstone)
# capstone builds both `capstone` (shared) and `capstone_static`; link the static one.
if(TARGET capstone_static)
    target_link_libraries(TracyCapstone INTERFACE capstone_static)
else()
    target_link_libraries(TracyCapstone INTERFACE capstone)
endif()

# zstd (trace files are zstd/lz4 compressed)
CPMAddPackage(
    NAME zstd
    GITHUB_REPOSITORY facebook/zstd
    GIT_TAG v1.5.7
    OPTIONS
        "ZSTD_BUILD_SHARED OFF"
        "ZSTD_BUILD_PROGRAMS OFF"
    EXCLUDE_FROM_ALL TRUE
    SOURCE_SUBDIR build/cmake
)
# server.cmake links the target name `libzstd`; map it to whatever zstd produced.
if(NOT TARGET libzstd)
    if(TARGET libzstd_static)
        add_library(libzstd ALIAS libzstd_static)
    elseif(TARGET zstd::libzstd_static)
        add_library(libzstd ALIAS zstd::libzstd_static)
    endif()
endif()

# PPQSort (parallel sort used by Worker statistics)
CPMAddPackage(
    NAME PPQSort
    GITHUB_REPOSITORY GabTux/PPQSort
    VERSION 1.0.6
    EXCLUDE_FROM_ALL TRUE
)

# nlohmann/json: vendored single header (avoids a flaky CPM git checkout).
add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json INTERFACE ${CMAKE_CURRENT_LIST_DIR}/third_party)

# --- Tracy compile flags + the Worker library -------------------------------
include(${TRACY_DIR}/cmake/config.cmake)
include(${TRACY_DIR}/cmake/server.cmake)

add_executable(tracy-mcp-query src/main.cpp)
target_link_libraries(tracy-mcp-query PRIVATE TracyServer nlohmann_json)
target_compile_features(tracy-mcp-query PRIVATE cxx_std_20)
