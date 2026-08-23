# The portable half. Built twice, from the same sources.
#
# Into RagdollSounds.dll by plugin/, and into the testbench exe by testbench/.
# It must not reference CommonLibVR: everything the game knows and the testbench
# does not arrives through IFeed as plain data, and everything the engine wants
# to play leaves through ICueSink.
#
# The guard is mechanical rather than a matter of discipline - see the
# check-portable target at the bottom, which fails the build on an RE/ or SKSE/
# include under core/.

cmake_minimum_required(VERSION 3.21)

project(RagdollSoundsCore VERSION 0.1.0 LANGUAGES CXX)

find_package(spdlog CONFIG REQUIRED)

file(GLOB_RECURSE RDS_CORE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
file(GLOB_RECURSE RDS_CORE_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h")

add_library(RagdollSoundsCore STATIC ${RDS_CORE_SOURCES} ${RDS_CORE_HEADERS})
add_library(rds::core ALIAS RagdollSoundsCore)

target_compile_features(RagdollSoundsCore PUBLIC cxx_std_23)
target_include_directories(RagdollSoundsCore PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
# ws2_32 for Link.cpp, the loopback socket between the game and the testbench.
# Both halves of the build get it, because both halves speak the protocol.
target_link_libraries(RagdollSoundsCore PUBLIC spdlog::spdlog ws2_32)
target_compile_definitions(RagdollSoundsCore PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

if(MSVC)
    target_compile_options(RagdollSoundsCore PRIVATE /W4 /permissive- /Zc:preprocessor)
endif()

# Defined by the testbench build only. The engine never reads it; Log.h uses it
# to pull spdlog in directly, because inside the plugin CommonLibSSE's PCH has
# already done that.
option(RDS_CORE_STANDALONE "Building outside the game process" OFF)
if(RDS_CORE_STANDALONE)
    target_compile_definitions(RagdollSoundsCore PUBLIC RDS_STANDALONE)
endif()

# One grep, run every build. A single RE/ include under core/ is the whole
# design failing quietly - it would still compile inside the plugin and only
# break when the testbench tried to link.
add_custom_target(rds_core_portable_check ALL
    COMMAND ${CMAKE_COMMAND}
            -DRDS_CORE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckPortable.cmake
    COMMENT "core/ must not include RE/ or SKSE/")
add_dependencies(RagdollSoundsCore rds_core_portable_check)

# A console harness that runs Verify() over a folder of recordings. Off inside
# the plugin - it would be dead weight in the DLL's build - and on by default in
# the standalone build, where it is the only way to check the backend before the
# testbench exists.
option(RDS_BUILD_TOOLS "Build the console harnesses" ${RDS_CORE_STANDALONE})
if(RDS_BUILD_TOOLS)
    add_executable(rds-verify "${CMAKE_CURRENT_SOURCE_DIR}/tools/rds-verify.cpp")
    target_link_libraries(rds-verify PRIVATE RagdollSoundsCore)
    target_compile_features(rds-verify PRIVATE cxx_std_23)
    if(MSVC)
        target_compile_options(rds-verify PRIVATE /W4 /permissive- /Zc:preprocessor)
    endif()
endif()
