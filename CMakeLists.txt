cmake_minimum_required(VERSION 3.25)
project(ambrosia LANGUAGES C CXX)
# For the programs' icon resource (src/ambrosia.rc).
if(WIN32)
  enable_language(RC)
endif()

# Compiler configuration
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Multi-config generators such as Visual Studio pick the configuration at build time.
get_property(DEUSEX_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT DEUSEX_MULTI_CONFIG AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE
        Release
        CACHE STRING
        "Choose the build type (Debug or Release)"
        FORCE
    )
endif()

# Link the C and C++ runtime statically, as the MinGW build does, so the programs run without the
# Visual C++ redistributable. Every target gets it, the vendored libraries too, as a link cannot mix
# runtimes. MSVC's ASan still uses its own runtime DLL.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
option(AMBROSIA_RELEASE_SYMBOLS "Generate matching PDBs for the x64 MSVC distribution" OFF)
if(AMBROSIA_RELEASE_SYMBOLS)
    if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "The Windows distribution requires a 64-bit MSVC build")
    endif()
    set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT Embedded)
    add_link_options(/DEBUG:FULL /INCREMENTAL:NO /OPT:REF /OPT:ICF)
endif()

# ccache, when it is installed, unless a compiler launcher was given (-DCMAKE_CXX_COMPILER_LAUNCHER=...)
# or ccache is turned off with -DAMBROSIA_CCACHE=OFF.
option(AMBROSIA_CCACHE "Build through ccache when it is installed" ON)
if(AMBROSIA_CCACHE AND NOT CMAKE_C_COMPILER_LAUNCHER AND NOT CMAKE_CXX_COMPILER_LAUNCHER)
    find_program(CCACHE_PROGRAM ccache)
    if(CCACHE_PROGRAM)
        # Plain variables, so a launcher given in the cache later still wins.
        set(CMAKE_C_COMPILER_LAUNCHER ${CCACHE_PROGRAM})
        set(CMAKE_CXX_COMPILER_LAUNCHER ${CCACHE_PROGRAM})
        # ccache cannot cache a file built with a precompiled header, and SDL builds all of its
        # sources with one; without it they are cached like the rest.
        set(CMAKE_DISABLE_PRECOMPILE_HEADERS ON)
    endif()
endif()

# Project options
option(SKIP_UT "Skip building the unit tests, for faster builds" OFF)
# No path to the game data is compiled in: the programs and the tests look at run time, in the
# DEUSEX_GAME_DATA environment variable and then a game-data folder beside the program. A
# development build then looks in any folder above it (game-data in the checkout, for a build under
# build/<preset>) and nowhere else; a shipped build looks in the system's data folders and then in
# Steam's libraries (src/core/desktop/platform.cc).
option(AMBROSIA_DEV_BUILD "Find game-data in the folders above the program, and never in the system's data folders or Steam; off for a shipped build" ON)
if(AMBROSIA_DEV_BUILD)
    add_compile_definitions(AMBROSIA_DEV_BUILD)
endif()
option(AMBROSIA_DATA_TESTS "Register the tests that need the game data, which fail without it; turn off where there is none" ON)

# Sanitizers
set(SANITIZER "none" CACHE STRING "Sanitizer to build with: none, asan, tsan")
set_property(CACHE SANITIZER PROPERTY STRINGS none asan tsan)
if(NOT SANITIZER MATCHES "^(none|asan|tsan)$")
    message(FATAL_ERROR "SANITIZER must be one of: none, asan, tsan (got '${SANITIZER}')")
endif()
if(MSVC AND SANITIZER STREQUAL "tsan")
    message(FATAL_ERROR "MSVC has no thread sanitizer; use SANITIZER=asan, or GCC or Clang for tsan")
endif()
if(MSVC AND SANITIZER STREQUAL "asan")
    # Every target gets it, the vendored libraries too, as the STL's container annotations must
    # match across a link. ASan does not work with /RTC or incremental linking.
    add_compile_options(/fsanitize=address)
    add_link_options(/INCREMENTAL:NO /DEBUG)
    set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT ProgramDatabase)
    foreach(flags CMAKE_C_FLAGS_DEBUG CMAKE_CXX_FLAGS_DEBUG)
        string(REGEX REPLACE "/RTC[1csu]*" "" ${flags} "${${flags}}")
    endforeach()
    # The runtime DLL is copied next to each program (see deusex_target_options).
    get_filename_component(DEUSEX_MSVC_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
    file(GLOB DEUSEX_ASAN_RUNTIME "${DEUSEX_MSVC_BIN}/clang_rt.asan_dynamic-x86_64.dll")
    if(NOT DEUSEX_ASAN_RUNTIME)
        message(FATAL_ERROR "clang_rt.asan_dynamic-x86_64.dll not found in ${DEUSEX_MSVC_BIN}")
    endif()
endif()

# Dependencies
set(BUILD_SHARED_LIBS OFF)

# Nintendo Switch homebrew (NINTENDO_SWITCH comes from devkitPro's toolchain file) builds only the
# game. char is signed there, as on x64, where it already is.
if(NINTENDO_SWITCH)
    add_compile_options(-fsigned-char)
    # Release is -O3 everywhere: CMake's own for GCC and Clang, and here in place of devkitPro's -O2,
    # which made frames 6-8% longer with LTO (docs/switch-performance/cpu-and-scripts.md).
    foreach(language C CXX)
        string(REPLACE "-O2" "-O3" CMAKE_${language}_FLAGS_RELEASE "${CMAKE_${language}_FLAGS_RELEASE}")
    endforeach()
endif()

# Link-time optimisation, on by default for the Switch only. On the desktop it made frames about
# 1% shorter but each rebuild after an edit 7 s longer (an 8 s link), so it is left for release
# packages there. Sanitizer builds never take it.
if(NINTENDO_SWITCH)
    set(DX_LTO_DEFAULT ON)
else()
    set(DX_LTO_DEFAULT OFF)
endif()
option(AMBROSIA_LTO "Link-time optimisation" ${DX_LTO_DEFAULT})
if(AMBROSIA_LTO)
    if(NOT SANITIZER STREQUAL "none")
        message(FATAL_ERROR "AMBROSIA_LTO does not go with SANITIZER=${SANITIZER}; turn one off")
    endif()
    include(CheckIPOSupported)
    check_ipo_supported(RESULT DX_LTO_SUPPORTED OUTPUT DX_LTO_ERROR LANGUAGES C CXX)
    if(NOT DX_LTO_SUPPORTED)
        message(FATAL_ERROR "AMBROSIA_LTO: the compiler cannot do link-time optimisation: ${DX_LTO_ERROR}")
    endif()
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
    # With LTO the code is generated at the link, so the flags that change it go there too.
    if(NINTENDO_SWITCH)
        add_link_options(-fsigned-char)
    endif()
endif()
message(STATUS "Release flags ${CMAKE_CXX_FLAGS_RELEASE}; LTO ${AMBROSIA_LTO}")

# The platform whose code is built: each layer's code for it is in a subdirectory of that name
# (src/core/desktop, src/game/switch and so on), which src/CMakeLists.txt adds to the layer.
if(NINTENDO_SWITCH)
    set(DX_PLATFORM switch)
else()
    set(DX_PLATFORM desktop)
endif()

# SDL3
if(NINTENDO_SWITCH)
    # devkitPro's SDL3 fork, which container/Containerfile.switch builds into the Switch portlibs.
    find_package(SDL3 REQUIRED CONFIG COMPONENTS SDL3-static)
    # The portlibs headers, SDL's among them, are third-party code as the vendored SDL is on
    # desktop, so the project's warnings stay out of their macros. -isystem overrides the
    # toolchain's plain -I for the same directory.
    include_directories(SYSTEM ${DEVKITPRO}/portlibs/switch/include)
    # Optional: a newer Mesa from mesa-switch in place of devkitPro's 20.1, which
    # container/Containerfile.switch-mesa installs into the portlibs (`x build switch
    # --mesa`). SDL names only -lEGL -lglapi -ldrm_nouveau; that libEGL also needs Mesa's own
    # utility archives, which its OpenGLConfig.cmake lists as OpenGL::EGL.
    option(AMBROSIA_SWITCH_MESA "Link the mesa-switch Mesa that Containerfile.switch-mesa installs" OFF)
    if(AMBROSIA_SWITCH_MESA)
        find_package(OpenGL REQUIRED CONFIG)
    endif()
else()
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
    # Parts of SDL the programs do not use: they draw with OpenGL through glad (ImGui's SDL backend
    # only keeps an SDL_Renderer pointer), and use no camera, file dialog, tray, battery, haptics,
    # sensors (a gamepad's gyro comes through the joystick drivers), asynchronous file reads or
    # KMS/DRM output. FriBidi and libthai only lay out text in SDL's own X11 message boxes.
    foreach(option SDL_RENDER SDL_GPU SDL_VULKAN SDL_OPENGLES SDL_CAMERA SDL_DIALOG SDL_TRAY SDL_POWER
            SDL_HAPTIC SDL_SENSOR SDL_LIBURING SDL_KMSDRM SDL_FRIBIDI SDL_LIBTHAI)
        set(${option} OFF CACHE BOOL "" FORCE)
    endforeach()
    add_subdirectory(${CMAKE_SOURCE_DIR}/vendor/SDL3-3.4.20 SYSTEM)
endif()

# glad
add_library(glad STATIC ${CMAKE_SOURCE_DIR}/vendor/glad-gl33/src/gl.c)
target_include_directories(glad SYSTEM PUBLIC ${CMAKE_SOURCE_DIR}/vendor/glad-gl33/include)

# miniaudio, only for decoding the sounds' WAV and MP3 data (decodeBytes in src/audio/audio.cc): they
# play through SDL (src/audio/output.cc), so its device code and the backends it loads at run time,
# its FLAC decoder, its engine, node graph and resource manager, and its threads are left out.
add_library(miniaudio STATIC ${CMAKE_SOURCE_DIR}/vendor/miniaudio/miniaudio.c)
target_include_directories(miniaudio SYSTEM PUBLIC ${CMAKE_SOURCE_DIR}/vendor/miniaudio)
target_compile_definitions(miniaudio PUBLIC MA_NO_DEVICE_IO MA_NO_RUNTIME_LINKING MA_NO_ENCODING MA_NO_GENERATION
    MA_NO_FLAC MA_NO_ENGINE MA_NO_NODE_GRAPH MA_NO_RESOURCE_MANAGER MA_NO_THREADING)
find_package(Threads REQUIRED)
target_link_libraries(miniaudio PUBLIC Threads::Threads)
if(NOT MSVC)
    target_link_libraries(miniaudio PUBLIC m)
endif()

# stb_image
add_library(stb_image STATIC ${CMAKE_SOURCE_DIR}/vendor/stb/stb_image.c)
target_include_directories(stb_image SYSTEM PUBLIC ${CMAKE_SOURCE_DIR}/vendor/stb)

# CLI11, the programs' command lines. A single header, included only by the files that parse them.
add_library(cli11 INTERFACE)
target_include_directories(cli11 SYSTEM INTERFACE ${CMAKE_SOURCE_DIR}/vendor/CLI11-2.7.2)

# Steam Audio, for the optional HRTF sound mode. Its prebuilt libraries are 64-bit x86 Windows and
# Linux only, so the Switch build never has it.
if(NOT NINTENDO_SWITCH AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|x86_64|amd64)$" AND (WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Linux"))
    set(DEUSEX_STEAMAUDIO_DEFAULT ON)
else()
    set(DEUSEX_STEAMAUDIO_DEFAULT OFF)
endif()
option(AMBROSIA_STEAM_AUDIO "Download Steam Audio and build the optional HRTF sound mode" ${DEUSEX_STEAMAUDIO_DEFAULT})
if(NINTENDO_SWITCH AND AMBROSIA_STEAM_AUDIO)
    message(FATAL_ERROR "Steam Audio has no Switch library; configure the Switch build with -DAMBROSIA_STEAM_AUDIO=OFF.")
endif()
if(AMBROSIA_STEAM_AUDIO)
    include(${CMAKE_SOURCE_DIR}/cmake/steamaudio.cmake)
endif()

# Tracy, the frame profiler, for profiling builds only (`x build --tracy`). Off, nothing of it
# is downloaded or built and core/profile.h's macros are empty.
option(AMBROSIA_TRACY "Download the Tracy profiler's client and build the programs with its zones" OFF)
if(AMBROSIA_TRACY)
    include(${CMAKE_SOURCE_DIR}/cmake/tracy.cmake)
    # Optimised as Release, with debug information so the viewer can show the source of a zone.
    if(MSVC)
        set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT Embedded)
        add_link_options(/DEBUG /OPT:REF /OPT:ICF)
    else()
        add_compile_options(-g)
    endif()
endif()

# LICENSES.txt, after Steam Audio and Tracy, whose licences it takes in.
include(${CMAKE_SOURCE_DIR}/cmake/licenses.cmake)

# Dear ImGui, for the editor and the game's developer overlay, with ImPlot (v1.1) for its plots
add_library(imgui STATIC
    ${CMAKE_SOURCE_DIR}/vendor/imgui/imgui.cpp
    ${CMAKE_SOURCE_DIR}/vendor/imgui/imgui_draw.cpp
    ${CMAKE_SOURCE_DIR}/vendor/imgui/imgui_tables.cpp
    ${CMAKE_SOURCE_DIR}/vendor/imgui/imgui_widgets.cpp
    ${CMAKE_SOURCE_DIR}/vendor/imgui/backends/imgui_impl_sdl3.cpp
    ${CMAKE_SOURCE_DIR}/vendor/imgui/backends/imgui_impl_opengl3.cpp
    ${CMAKE_SOURCE_DIR}/vendor/implot/implot.cpp
    ${CMAKE_SOURCE_DIR}/vendor/implot/implot_items.cpp
)
target_include_directories(imgui SYSTEM PUBLIC ${CMAKE_SOURCE_DIR}/vendor/imgui ${CMAKE_SOURCE_DIR}/vendor/imgui/backends ${CMAKE_SOURCE_DIR}/vendor/implot)
# ImPlot instantiates every plot for ten numeric types by default, which made implot_items.cpp the
# slowest file in the build at -O3. The plots plot only doubles; a plot of another type fails to link.
target_compile_definitions(imgui PUBLIC "IMPLOT_CUSTOM_NUMERIC_TYPES=(float)(double)")
target_link_libraries(imgui PUBLIC SDL3::SDL3-static)
# ImGui's own GL loader opens the GL library with dlopen, which libnx does not have; on the Switch
# the backend calls GL through glad instead, which the game has loaded before the overlay starts.
# Nor is there a shell to open links in.
if(NINTENDO_SWITCH)
    target_compile_definitions(imgui PRIVATE IMGUI_IMPL_OPENGL_LOADER_CUSTOM IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS)
    target_compile_options(imgui PRIVATE -include glad/gl.h)
    target_link_libraries(imgui PRIVATE glad)
    # newlib has no timegm, which ImPlot's date axes use.
    set_source_files_properties(${CMAKE_SOURCE_DIR}/vendor/implot/implot.cpp PROPERTIES COMPILE_OPTIONS "-include;${CMAKE_SOURCE_DIR}/src/debugui/switch_timegm.h")
endif()

# The editor's and the tests' libraries, which the Switch build does not need.
if(NOT NINTENDO_SWITCH)
    # ImGuizmo
    add_library(imguizmo STATIC ${CMAKE_SOURCE_DIR}/vendor/imguizmo/ImGuizmo.cpp)
    target_include_directories(imguizmo SYSTEM PUBLIC ${CMAKE_SOURCE_DIR}/vendor/imguizmo)
    target_link_libraries(imguizmo PUBLIC imgui)

    # Catch
    add_library(Catch2 STATIC ${CMAKE_SOURCE_DIR}/vendor/catch-3.16.1/catch_amalgamated.cpp)
    target_include_directories(Catch2 SYSTEM PUBLIC ${CMAKE_SOURCE_DIR}/vendor/catch-3.16.1)
endif()

# Add targets
add_subdirectory(src)

if(NOT SKIP_UT AND NOT NINTENDO_SWITCH)
    enable_testing()
    add_subdirectory(tests/unit)
endif()
