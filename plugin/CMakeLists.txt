cmake_minimum_required(VERSION 3.13)
project(speed_editor_ctrl)

# ── SDR++ out-of-tree module build ──────────────────────────────────
#
# Required variables (set via -D on the cmake command line):
#   SDRPP_SOURCE      — path to the SDR++ source tree  (e.g. ~/SDRPlusPlus)
#   SDRPP_BUILD       — path to an existing SDR++ build dir (e.g. ~/SDRPlusPlus/build)
#
# Example:
#   cmake -B build \
#         -DSDRPP_SOURCE=$HOME/SDRPlusPlus \
#         -DSDRPP_BUILD=$HOME/SDRPlusPlus/build
#   cmake --build build

if (NOT SDRPP_SOURCE)
    message(FATAL_ERROR "SDRPP_SOURCE must be set to the SDR++ source tree root")
endif()
if (NOT SDRPP_BUILD)
    message(FATAL_ERROR "SDRPP_BUILD must be set to an existing SDR++ build directory")
endif()

set(SDRPP_CORE_ROOT "${SDRPP_SOURCE}/core/src")

# Create an IMPORTED target for sdrpp_core so the module cmake's
# target_link_libraries() resolves without an in-tree build.
add_library(sdrpp_core SHARED IMPORTED)
set_target_properties(sdrpp_core PROPERTIES
    IMPORTED_LOCATION "${SDRPP_BUILD}/core/libsdrpp_core.so"
    INTERFACE_INCLUDE_DIRECTORIES "${SDRPP_CORE_ROOT};${SDRPP_CORE_ROOT}/imgui;${SDRPP_SOURCE}/core/backends/glfw;${SDRPP_SOURCE}/core/backends/glfw/imgui;${SDRPP_SOURCE}/core/libcorrect/include"
)

# The generated module cmake: creates the shared lib, links sdrpp_core,
# adds include dirs, sets compiler flags.
set(SDRPP_MODULE_CMAKE "${SDRPP_BUILD}/sdrpp_module.cmake")

# ── Plugin sources ──────────────────────────────────────────────────

find_package(PkgConfig REQUIRED)
pkg_check_modules(HIDAPI REQUIRED hidapi-libusb)

file(GLOB SRC "src/*.cpp")

include(${SDRPP_MODULE_CMAKE})

target_include_directories(speed_editor_ctrl PRIVATE
    "src/"
    ${HIDAPI_INCLUDE_DIRS}
    "${SDRPP_SOURCE}/decoder_modules/radio/src/"
    "${SDRPP_SOURCE}/misc_modules/recorder/src/"
)
target_link_libraries(speed_editor_ctrl PRIVATE ${HIDAPI_LIBRARIES})
