# Official devkitPro Nintendo Switch cross-toolchain. Desktop builds never use it.
if(DEFINED ENV{DEVKITPRO})
    set(NIGHT100_DEVKITPRO "$ENV{DEVKITPRO}")
else()
    set(NIGHT100_DEVKITPRO /opt/devkitpro)
endif()
set(ENV{DEVKITPRO} "${NIGHT100_DEVKITPRO}")
include("${NIGHT100_DEVKITPRO}/cmake/Switch.cmake")
