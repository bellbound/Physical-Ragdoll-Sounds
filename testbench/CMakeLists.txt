# The throwaway half.
#
# Not shipped, not registered in mod-registry.ps1, and deliberately kept out of
# the plugin's build so nothing here can drift into the mod. Its only contact
# with the backend is core/'s public headers - Offline.h, ConfigManager.h,
# Recording.h - and if it ever needs more than that, the seam is wrong.
#
# Build:
#   cmake --preset testbench && cmake --build build/testbench

cmake_minimum_required(VERSION 3.21)

project(RagdollSoundsTestbench VERSION 0.1.0 LANGUAGES CXX)

find_package(glfw3 CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(Stb REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(OpenGL REQUIRED)

# Two single-header libraries, vendored rather than taken from vcpkg. Fetched on
# the first configure rather than committed - together they are five megabytes
# and this program is throwaway.
#
# miniaudio is not in vcpkg at all. nlohmann-json is, but its port cannot be
# built underneath this source tree: vcpkg puts its build tree beside the binary
# directory, that path already contains a space and is 120 characters deep, and
# MSVC answers with "Cannot open compiler generated file: Invalid argument". The
# library is one header and needs no build, so taking the header is both the
# smaller dependency and the one that works.
function(rds_vendor_header RELATIVE URL)
    set(TARGET_FILE "${CMAKE_CURRENT_SOURCE_DIR}/external/${RELATIVE}")
    if(EXISTS "${TARGET_FILE}")
        return()
    endif()
    message(STATUS "Fetching ${RELATIVE}")
    file(DOWNLOAD "${URL}" "${TARGET_FILE}" STATUS RDS_STATUS)
    list(GET RDS_STATUS 0 RDS_RC)
    if(NOT RDS_RC EQUAL 0)
        file(REMOVE "${TARGET_FILE}")
        message(FATAL_ERROR "could not download ${RELATIVE}: ${RDS_STATUS}")
    endif()
endfunction()

rds_vendor_header("miniaudio.h"
    "https://raw.githubusercontent.com/mackron/miniaudio/master/miniaudio.h")
# Obs.cpp speaks obs-websocket, which is JSON over a plain ws:// socket on
# loopback. WinHTTP carries the socket; this carries the messages.
rds_vendor_header("nlohmann/json.hpp"
    "https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp")

# The engine, built with no game around it. RDS_CORE_STANDALONE only tells Log.h
# to include spdlog itself, since there is no CommonLibSSE PCH to have done it.
set(RDS_CORE_STANDALONE ON CACHE BOOL "" FORCE)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../core" "${CMAKE_CURRENT_BINARY_DIR}/core")

file(GLOB_RECURSE TB_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
file(GLOB_RECURSE TB_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h")

add_executable(RagdollSoundsTestbench ${TB_SOURCES} ${TB_HEADERS})

target_compile_features(RagdollSoundsTestbench PRIVATE cxx_std_23)
target_include_directories(RagdollSoundsTestbench PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/external   # miniaudio.h and nlohmann/json.hpp, vendored
    ${Stb_INCLUDE_DIR})

target_link_libraries(RagdollSoundsTestbench PRIVATE
    rds::core
    glfw
    imgui::imgui
    spdlog::spdlog
    OpenGL::GL
    # Obs.cpp: winhttp for the websocket, bcrypt for the handshake SHA256,
    # crypt32 for base64.
    winhttp
    bcrypt
    crypt32)

target_compile_definitions(RagdollSoundsTestbench PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

if(MSVC)
    target_compile_options(RagdollSoundsTestbench PRIVATE /W4 /permissive- /Zc:preprocessor)
endif()

# Where everything lives by default, so the exe finds it all with no arguments.
#
# The library and the sfx ini are the two that are not under testbench/. The
# library is assets/sfx/library because that is where the sounds themselves are
# authored and kept, and the ini goes straight into the deployed mod because
# unlike an algorithm config there is only ever one of it - there is nothing to
# A/B between and nowhere else for it to be true.
target_compile_definitions(RagdollSoundsTestbench PRIVATE
    RDS_DEFAULT_RECORDINGS="${CMAKE_CURRENT_SOURCE_DIR}/../Research/NewRecordings"
    RDS_DEFAULT_CONFIGS="${CMAKE_CURRENT_SOURCE_DIR}/configs"
    RDS_DEFAULT_FRAMECACHE="${CMAKE_CURRENT_SOURCE_DIR}/framecache"
    RDS_DEFAULT_LIBRARY="${CMAKE_CURRENT_SOURCE_DIR}/../assets/sfx/library"
    # An extract of the game's sound/fx tree, for playing a take's vanilla track
    # back. Bethesda's files, so it sits beside assets/sfx/skyrim rather than
    # inside library/ - outside everything deploy-pack.ps1 can reach, for the
    # three reasons that folder's README sets out. Absent by default, and that is
    # fine: the vanilla side is then silent and the switch says why.
    RDS_DEFAULT_VANILLA_SOUNDS="${CMAKE_CURRENT_SOURCE_DIR}/../assets/sfx/vanilla"
    RDS_DEFAULT_SFX_INI="${CMAKE_CURRENT_SOURCE_DIR}/../../../papyrus/mods/Physical Ragdoll Sounds/deployment_files/main/SKSE/Plugins/RagdollSounds/RagdollSounds_SFX.ini")
