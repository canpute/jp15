cmake_minimum_required(VERSION 3.17)

project(ojph_capi
    VERSION 0.0.0
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
# ---------------------------------------------------------------------------
set(OJPH_BUILD_EXECUTABLES OFF CACHE BOOL "" FORCE)
set(OJPH_BUILD_TESTS       OFF CACHE BOOL "" FORCE)
add_subdirectory(OpenJPH)

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
# Linking PRIVATE means OpenJPH symbols are NOT re-exported from our .so.
target_link_libraries(ojph_capi PRIVATE openjph)


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
