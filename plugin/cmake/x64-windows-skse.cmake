set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_BUILD_TYPE release)

# Workaround: CMake 3.30 generates manifest.rc that rc.exe can't parse (RC2136)
set(VCPKG_LINKER_FLAGS "/MANIFEST:NO")
set(VCPKG_LINKER_FLAGS_RELEASE "/MANIFEST:NO")
set(VCPKG_LINKER_FLAGS_DEBUG "/MANIFEST:NO")

if (${PORT} MATCHES "fully-dynamic-game-engine|skse|qt*")
    set(VCPKG_LIBRARY_LINKAGE dynamic)
else ()
    set(VCPKG_LIBRARY_LINKAGE static)
endif ()
