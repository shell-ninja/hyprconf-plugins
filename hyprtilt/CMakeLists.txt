cmake_minimum_required(VERSION 3.25)
project(hyprtilt LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(HYPRLAND REQUIRED IMPORTED_TARGET hyprland)

add_library(hyprtilt SHARED
    src/TiltTransformer.cpp
    src/main.cpp
)

target_include_directories(hyprtilt PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(hyprtilt PRIVATE
    PkgConfig::HYPRLAND
    GLESv2
    EGL
)

target_compile_options(hyprtilt PRIVATE
    -Wall
    -Wextra
    -O3
    -fPIC
    # Required so dlclose() can actually unload the plugin. Inline variables
    # otherwise become STB_GNU_UNIQUE symbols, which mark the DSO NODELETE.
    -fno-gnu-unique
)
# Do NOT set -fvisibility=hidden here: Hyprland's globals (g_pInputManager,
# g_pHyprRenderer, ...) are inline variables, and hiding the plugin's weak
# definitions makes it bind to private null copies.

set_target_properties(hyprtilt PROPERTIES
    PREFIX ""
    OUTPUT_NAME "hyprtilt"
)

# Install location. The user-local layout used by install.sh is the default;
# an explicit system prefix installs into <prefix>/lib/hyprland, the
# conventional location for Hyprland plugins. Override HYPRTILT_INSTALL_DIR
# when packaging for a different layout.
set(HYPRTILT_INSTALL_DIR "" CACHE PATH "Directory where hyprtilt.so is installed")
if(NOT HYPRTILT_INSTALL_DIR)
    if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
        set(HYPRTILT_INSTALL_DIR "$ENV{HOME}/.local/share/hyprconf/plugins")
    else()
        set(HYPRTILT_INSTALL_DIR "${CMAKE_INSTALL_PREFIX}/lib/hyprland")
    endif()
endif()

install(TARGETS hyprtilt DESTINATION "${HYPRTILT_INSTALL_DIR}")
