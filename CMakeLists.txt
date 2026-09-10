cmake_minimum_required(VERSION 3.27)

project(aura_titlebar
    DESCRIPTION "aura-titlebar plugin for Hyprland"
    VERSION 0.1
)

set(CMAKE_CXX_STANDARD 23)

set(SRC main.cpp barDeco.cpp BarPassElement.cpp TitlebarBlur.cpp)

add_library(aura_titlebar SHARED ${SRC})

# Mirrors upstream hyprbars' Makefile:
# - INCLUDES = pkg-config --cflags ... (headers only)
# - LIBS = (empty). Plugin symbols are resoled by the running compositor;
#   linking aquamarine/hyprutils etc. at build time makes the dynamic loader
#   abort inside dlsym when Hyprland loads the plugin.
find_package(PkgConfig REQUIRED)
pkg_check_modules(deps REQUIRED IMPORTED_TARGET
    hyprland
    libdrm
    libinput
    libudev
    pixman-1
    wayland-server
    xkbcommon
)
target_include_directories(aura_titlebar PRIVATE ${deps_INCLUDE_DIRS})
target_link_libraries(aura_titlebar PRIVATE rt)

# GNU unique binding is chosen while compiling, not while linking existing
# objects. It prevents safe symbol isolation between compositor and plugin.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(aura_titlebar PRIVATE -fno-gnu-unique)
endif()

install(TARGETS aura_titlebar)
