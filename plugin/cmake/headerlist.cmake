# The menu is one page per file plus one integration per mod, so the roster moves
# every time a page is added. A CONFIGURE_DEPENDS glob keeps it honest without a
# build edit per file.
file(GLOB_RECURSE headers CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
)
