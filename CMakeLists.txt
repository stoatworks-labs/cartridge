cmake_minimum_required(VERSION 3.15)

project(Cartridge
    VERSION 0.1.2
    LANGUAGES C CXX
)

# ---------------------------------------------------------------------------
# Universal by default so one bundle loads in both the Apple Silicon and the
# Intel build of Resolume. Override with -DCMAKE_OSX_ARCHITECTURES=arm64 for a
# faster dev build.
#
# This must sit immediately after project() and before any target exists. CMake
# latches the architecture list when the first target is created; setting it
# later is silently ignored and you get an arm64-only binary that the build log
# calls a success. Verify with `lipo`, never with the log.
# ---------------------------------------------------------------------------
if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)

# Apple deprecated the whole OpenGL API in 10.14 and FFGL's plugin interface is
# still OpenGL, so every SDK call warns. There is no alternative API to move to
# here, so silencing it is not ignoring a problem.
if(APPLE)
    add_compile_definitions(GL_SILENCE_DEPRECATION)
endif()

if(MSVC)
    add_compile_options(/utf-8)
endif()

# windows.h defines min and max as macros, which turns every std::min into
# `std::(...)` and fails with an error naming neither min nor windows.h.
if(WIN32)
    add_compile_definitions(NOMINMAX)
endif()

option(CARTRIDGE_BUILD_TOOLS  "Build the test core and the offline harness" ON)
option(CARTRIDGE_BUILD_PLUGIN "Build the FFGL plugin (needs the FFGL submodule)" ON)
option(CARTRIDGE_BUILD_HELPER "Build the out-of-process helper" ON)

# ---------------------------------------------------------------------------
# The libretro host. Everything that is not FFGL and not the helper lives here,
# because both builds are the same frontend differing only in where the texture
# ends up -- see AGENTS.md.
#
# OBJECT rather than STATIC, for the same reason as the rest of the fleet: the
# FFGL plugin registers itself from a file-scope constructor that nothing
# references by name, and in an archive the linker may drop the whole
# translation unit. That gives a bundle which loads, exports plugMain, and
# reports that it contains no plugins.
# ---------------------------------------------------------------------------
add_library(cartridge_core OBJECT
    source/Diag.h
    source/Diag.cpp
    source/Frame.h
    source/Input.h
    source/Audio.h
    source/Pixels.h
    source/Pixels.cpp
    source/Core.h
    source/Core.cpp
    source/Runner.h
    source/Paths.cpp
    source/Runner.cpp
    source/Shared.h
    source/Shared.cpp
)

target_include_directories(cartridge_core PUBLIC source external)
target_compile_definitions(cartridge_core PUBLIC CARTRIDGE_VERSION="${PROJECT_VERSION}")
set_target_properties(cartridge_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(NOT WIN32)
    # dlopen/dlsym. Not a no-op on Linux; harmless on macOS where they are in libc.
    target_link_libraries(cartridge_core PUBLIC ${CMAKE_DL_LIBS})
endif()

find_package(Threads REQUIRED)
target_link_libraries(cartridge_core PUBLIC Threads::Threads)

# ---------------------------------------------------------------------------
# The synthetic core, and the harness that drives it.
#
# The test core is a real libretro core -- a MODULE loaded through the same
# dlopen path as any other. It links nothing of ours on purpose: a test core
# that shared our headers could agree with our own mistakes.
# ---------------------------------------------------------------------------
if(CARTRIDGE_BUILD_TOOLS)
    add_library(cartridge_testcore MODULE tools/testcore/testcore.c)
    target_include_directories(cartridge_testcore PRIVATE external)
    set_target_properties(cartridge_testcore PROPERTIES
        PREFIX ""
        OUTPUT_NAME "cartridge_testcore"
    )
    if(APPLE)
        # .dylib rather than CMake's default .so for a MODULE, so it looks like
        # every other core on the machine and dlopen finds it by the name the
        # harness builds.
        set_target_properties(cartridge_testcore PROPERTIES SUFFIX ".dylib")
    endif()

    # ZLIB::ZLIB, not a bare `z`. The harness writes its own PNGs and zlib does
    # ship with macOS and Linux, but naming the library directly finds nothing
    # on Windows -- the build fails at `#include <zlib.h>` in a test tool, long
    # after the plugin itself has linked. find_package resolves the system copy
    # on Unix and the vcpkg one on Windows.
    find_package(ZLIB REQUIRED)
    add_executable(cartest tools/cartest/main.cpp)
    target_link_libraries(cartest PRIVATE cartridge_core ZLIB::ZLIB)
    add_dependencies(cartest cartridge_testcore)
endif()

# ---------------------------------------------------------------------------
# The out-of-process build. Same libretro host, different destination for the
# pixels -- see AGENTS.md. Not gated on the FFGL SDK: the helper has no GL and
# no plugin dependency at all, which is exactly why a core crashing in it
# cannot take a host down.
# ---------------------------------------------------------------------------
if(CARTRIDGE_BUILD_HELPER)
    add_executable(cartridge-helper tools/helper/main.cpp)
    target_link_libraries(cartridge-helper PRIVATE cartridge_core)
    if(NOT WIN32)
        # shm_open/shm_unlink live in librt on Linux; on macOS they are in libc.
        if(NOT APPLE)
            target_link_libraries(cartridge-helper PRIVATE rt)
        endif()
    endif()
endif()

# ---------------------------------------------------------------------------
# The FFGL plugin -- the in-process build. Runs the core inside Resolume.
# ---------------------------------------------------------------------------
if(CARTRIDGE_BUILD_PLUGIN AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/ffgl/CMakeLists.txt")
    set(FFGL_BUILD_EXAMPLE_PLUGINS OFF CACHE BOOL "" FORCE)
    add_subdirectory(external/ffgl EXCLUDE_FROM_ALL)

    # -----------------------------------------------------------------------
    # The plugin class, as an OBJECT library so the GL harness can link the
    # same objects the bundle does -- the harness must test the real class, not
    # a copy of it.
    #
    # Note what is NOT in here: SourcePlugin.cpp. That file carries the
    # `CFFGLPluginInfo` registration, and it is listed directly in the MODULE
    # target below. In a STATIC archive the linker is entitled to drop it (the
    # object is never referenced by name), giving a bundle that loads, exports
    # plugMain and reports that it contains no plugins. Keeping it out of the
    # shared library also keeps the harness from registering a plugin it has no
    # host for.
    # -----------------------------------------------------------------------
    add_library(cartridge_plugin OBJECT
        source/Plugin.h
        source/Plugin.cpp
        source/Controls.h
    )
    target_link_libraries(cartridge_plugin PUBLIC cartridge_core ffgl::sdk)
    set_target_properties(cartridge_plugin PROPERTIES POSITION_INDEPENDENT_CODE ON)

    if(NOT APPLE)
        find_package(GLEW REQUIRED)
        target_link_libraries(cartridge_plugin PUBLIC GLEW::GLEW)
    endif()

    # Both object libraries are named explicitly rather than relying on
    # cartridge_plugin to pull cartridge_core in behind it: CMake does not
    # propagate the *objects* of one OBJECT library through another, so a
    # transitive link builds and then fails at the final link with every symbol
    # from the inner library undefined.
    add_library(CartridgeSource MODULE source/SourcePlugin.cpp)
    target_link_libraries(CartridgeSource PRIVATE cartridge_plugin cartridge_core)

    if(APPLE)
        set_target_properties(CartridgeSource PROPERTIES
            BUNDLE TRUE
            BUNDLE_EXTENSION "bundle"
            OUTPUT_NAME "Cartridge"
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in"
            MACOSX_BUNDLE_BUNDLE_NAME "Cartridge"
            MACOSX_BUNDLE_GUI_IDENTIFIER "com.stoatworks.ffgl.cartridge"
            MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        )
        # FFGL hosts resolve plugMain by symbol name, so keep default visibility.
        target_link_libraries(CartridgeSource PRIVATE
            "-framework OpenGL"
            "-framework Carbon"
            "-framework AppKit"
        )
    else()
        set_target_properties(CartridgeSource PROPERTIES OUTPUT_NAME "Cartridge")
    endif()

    # -----------------------------------------------------------------------
    # The GL harness. Drives the real plugin class through the real FFGL
    # sequence in a headless core-profile context -- the only thing that
    # catches a shader that will not compile or a uniform name that does not
    # match the GLSL. macOS only: it is CGL.
    # -----------------------------------------------------------------------
    if(CARTRIDGE_BUILD_TOOLS AND APPLE)
        add_executable(cargl tools/cargl/main.cpp)
        target_link_libraries(cargl PRIVATE cartridge_plugin cartridge_core z
            "-framework OpenGL"
            "-framework AppKit"
        )
        target_include_directories(cargl PRIVATE tools)
        add_dependencies(cargl cartridge_testcore)
    endif()

    if(APPLE AND CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
        set(CMAKE_INSTALL_PREFIX "$ENV{HOME}/Documents/Resolume Arena/Extra Effects"
            CACHE PATH "" FORCE)
    endif()

    install(TARGETS CartridgeSource
        LIBRARY DESTINATION "."
        BUNDLE  DESTINATION "."
    )
elseif(CARTRIDGE_BUILD_PLUGIN)
    message(STATUS
        "FFGL SDK not present -- skipping the plugin. "
        "Run: git submodule update --init --recursive")
endif()
