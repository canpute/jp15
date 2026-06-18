cmake_minimum_required(VERSION 3.17)

project(ojph_capi
    VERSION 0.1.0
    DESCRIPTION "C shim over OpenJPH for cffi ABI-mode binding"
    LANGUAGES CXX C
)

# ---------------------------------------------------------------------------
# C++ standard — OpenJPH requires C++11; we use C++17 for std::clamp etc.
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Default build type
# ---------------------------------------------------------------------------
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    message(STATUS "No build type set; defaulting to Release")
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# OpenJPH via add_subdirectory (git submodule at ./OpenJPH)
#
# Key flags:
#   OJPH_BUILD_EXECUTABLES  OFF  — we don't need ojph_compress / ojph_expand
#   BUILD_SHARED_LIBS       OFF  — build OpenJPH as a static library so we
#                                  can fold it into our single .so
#
# v0.26.0 note: the source-tree header directory was renamed from "common"
# to "openjph", fixing the add_subdirectory include-path mismatch.  We
# therefore require v0.26.0 or later (submodule should be pinned there or
# newer).
# ---------------------------------------------------------------------------
set(OJPH_BUILD_EXECUTABLES OFF CACHE BOOL "" FORCE)
set(OJPH_BUILD_TESTS       OFF CACHE BOOL "" FORCE)

# Preserve the caller's BUILD_SHARED_LIBS, but force OpenJPH itself static.
set(_saved_shared ${BUILD_SHARED_LIBS})
set(BUILD_SHARED_LIBS OFF)
add_subdirectory(OpenJPH)
set(BUILD_SHARED_LIBS ${_saved_shared})

# ---------------------------------------------------------------------------
# Our C-shim shared library
# ---------------------------------------------------------------------------
add_library(ojph_capi SHARED
    src/ojph_capi.cpp
)

# The public header — installed alongside the .so so cffi can find it.
target_sources(ojph_capi PUBLIC
    FILE_SET HEADERS
    BASE_DIRS src
    FILES src/ojph_capi.h
)

# OpenJPH's CMake target is called "openjph" (lowercase).
# Linking PRIVATE means OpenJPH symbols are NOT re-exported from our .so,
# but because OpenJPH is static they are folded in.
target_link_libraries(ojph_capi PRIVATE openjph)

# ---------------------------------------------------------------------------
# Include paths
#
# After v0.26.0 the public headers live at:
#   OpenJPH/src/core/openjph/   (in-source-tree name matches install name)
#
# The openjph CMake target already propagates these via INTERFACE_INCLUDE_
# DIRECTORIES, so the target_link_libraries above is usually sufficient.
# We add an explicit fallback in case the propagation isn't set up yet
# in the version you're pinned to.
# ---------------------------------------------------------------------------
target_include_directories(ojph_capi PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/OpenJPH/src/core/openjph
    # pre-v0.26.0 fallback (harmless if the directory doesn't exist):
    ${CMAKE_CURRENT_SOURCE_DIR}/OpenJPH/src/core/common
)

# ---------------------------------------------------------------------------
# Compiler options
# ---------------------------------------------------------------------------
if(MSVC)
    target_compile_options(ojph_capi PRIVATE /W3 /WX-)
else()
    target_compile_options(ojph_capi PRIVATE
        -Wall -Wextra -Wno-unused-parameter
        # Silence the "missing field initializer" warning from C-struct
        # zero-init patterns used in the capi.
        -Wno-missing-field-initializers
    )
endif()

# ---------------------------------------------------------------------------
# Output name — cffi will dlopen "libojph_capi.so" / "ojph_capi.dll"
# ---------------------------------------------------------------------------
set_target_properties(ojph_capi PROPERTIES
    OUTPUT_NAME  ojph_capi
    # Avoid soname versioning for the Python wheel — cffi dlopens by exact
    # filename; the wheel packaging handles versioning at a higher level.
    NO_SONAME ON
)

# ---------------------------------------------------------------------------
# Platform-specific linker flags
# ---------------------------------------------------------------------------
if(APPLE)
    # Export all extern-C symbols (default on macOS anyway, but be explicit).
    target_link_options(ojph_capi PRIVATE -Wl,-export_dynamic)
elseif(UNIX)
    # Link libstdc++ statically where possible so the .so doesn't pick up
    # the host's libstdc++ version at cffi dlopen time.
    # (cibuildwheel's manylinux images already ship a controlled libstdc++,
    #  so this is most useful for local dev builds.)
    # Uncomment if you want fully self-contained wheels on Linux:
    # target_link_options(ojph_capi PRIVATE -static-libstdc++ -static-libgcc)
endif()

# ---------------------------------------------------------------------------
# Install rules (used by scikit-build-core / pip install)
# ---------------------------------------------------------------------------
include(GNUInstallDirs)

install(TARGETS ojph_capi
    LIBRARY  DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME  DESTINATION ${CMAKE_INSTALL_BINDIR}   # Windows DLL
    FILE_SET HEADERS DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# ---------------------------------------------------------------------------
# Optional: print a summary
# ---------------------------------------------------------------------------
message(STATUS "")
message(STATUS "=== ojph_capi build configuration ===")
message(STATUS "  Build type   : ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ standard : ${CMAKE_CXX_STANDARD}")
message(STATUS "  Install prefix: ${CMAKE_INSTALL_PREFIX}")
message(STATUS "")
