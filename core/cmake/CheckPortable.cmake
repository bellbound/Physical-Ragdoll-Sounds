# Fails the build if anything under core/ includes RE/ or SKSE/.
#
# The engine is compiled into the testbench exe with no game and no CommonLibVR.
# An RE/ include here still compiles inside the plugin, so without this the
# mistake only surfaces as a link error in the other build - by which point the
# code that made it is a week old.

file(GLOB_RECURSE RDS_FILES "${RDS_CORE_DIR}/include/*.h" "${RDS_CORE_DIR}/src/*.cpp" "${RDS_CORE_DIR}/src/*.h")

set(RDS_BAD "")
foreach(f ${RDS_FILES})
    file(READ "${f}" contents)
    if(contents MATCHES "#[ \t]*include[ \t]*[<\"](RE|SKSE)/")
        list(APPEND RDS_BAD "${f}")
    endif()
endforeach()

if(RDS_BAD)
    message(FATAL_ERROR
        "core/ is the portable half and must not include RE/ or SKSE/.\n"
        "Move the game-facing part into plugin/ behind IFeed or ICueSink.\n"
        "Offending files:\n  ${RDS_BAD}")
endif()
