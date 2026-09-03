cmake_minimum_required(VERSION 3.16)

# Bundled dependencies (glm, SDL2, json, bitsery, civetweb, eventpp) still declare
# cmake_minimum_required() < 3.5, which CMake 4.0+ rejects. This lets them configure
# without editing each submodule. Cascades into all add_subdirectory() children.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

project(scripter)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin")

# =============
# == OPTIONS ==
# =============
option(OFS_PROFILE OFF)

if(WIN32)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    # AVX is enabled by default on Windows (supported by all CPUs since 2011)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /arch:AVX")
endif()

# ====================
# === DEPENDENCIES ===
# ====================
add_subdirectory("lib/")

# =====================
# ==== BUILD-TOOLS ====
# =====================
add_subdirectory("localization/")

# ==============
# ==== SRC ====
# ==============
add_subdirectory("OFS-lib/")
add_subdirectory("src/")

