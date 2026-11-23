# CMake toolchain for Nintendo Wii U (devkitPPC, newlib, big-endian PPC750)
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR powerpc)
set(DEVKITPRO "$DEVKITPRO")
set(DAVE_ROOT "$DEPS/dave")
set(CMAKE_C_COMPILER   "${DEVKITPRO}/devkitPPC/bin/powerpc-eabi-gcc.exe")
set(CMAKE_CXX_COMPILER "${DEVKITPRO}/devkitPPC/bin/powerpc-eabi-g++.exe")
set(CMAKE_AR     "${DEVKITPRO}/devkitPPC/bin/powerpc-eabi-gcc-ar.exe" CACHE FILEPATH "")
set(CMAKE_RANLIB "${DEVKITPRO}/devkitPPC/bin/powerpc-eabi-gcc-ranlib.exe" CACHE FILEPATH "")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
# Cemu-safe code generation (see BUILD-NOTES.md "Cemu compatibility"):
#  * -O1: Cemu 2.6's PPC recompiler mis-executes some GCC 16 -O2/-O3 code (CR0 captured with mfcr
#    across calls in OpenSSL BN_usub -> wild write). Release flags are forced to -O1 below too.
#  * __builtin_trap -> abort: GCC 16 libstdc++ shared_ptr refcount-overflow checks compile to a
#    conditional trap (tweq); Cemu ignores the TO condition and halts the thread on ANY tw/twi.
set(WIIU_FLAGS "-O1 -ffunction-sections -fdata-sections -DESPRESSO -D__WIIU__ -D__WUT__ -mcpu=750 -meabi -mhard-float -D_GNU_SOURCE")
set(CMAKE_C_FLAGS_INIT   "${WIIU_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${WIIU_FLAGS} -fexceptions -frtti -D__builtin_trap()=__builtin_abort()")
set(CMAKE_C_FLAGS_RELEASE   "-O1 -DNDEBUG" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS_RELEASE "-O1 -DNDEBUG" CACHE STRING "" FORCE)
set(CMAKE_FIND_ROOT_PATH "${DAVE_ROOT}/install" "${DEVKITPRO}/portlibs/wiiu" "${DEVKITPRO}/portlibs/ppc" "${DEVKITPRO}/wut")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)
set(OPENSSL_ROOT_DIR "${DAVE_ROOT}/install" CACHE PATH "")
set(OPENSSL_USE_STATIC_LIBS ON)
