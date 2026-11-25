if(MSVC)
  set(DEUSEX_WARNINGS /W4 /WX /permissive- /Zc:preprocessor /utf-8)
else()
  set(DEUSEX_WARNINGS
    -Wall -Wextra -Wpedantic -Walloca -Wcast-align -Wcast-qual
    -Wconversion -Wdouble-promotion -Werror -Wformat=2
    -Wimplicit-fallthrough -Winit-self -Wnon-virtual-dtor
    -Wold-style-cast -Woverloaded-virtual
    -Wredundant-decls -Wshadow -Wsign-conversion -Wsuggest-override
    -Wswitch-enum -Wundef -Wuninitialized -Wunused
  )
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 13)
  list(APPEND DEUSEX_WARNINGS -Wno-restrict)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  list(APPEND DEUSEX_WARNINGS -Wno-double-promotion -Wno-unneeded-internal-declaration -Wno-unused-but-set-variable)
endif()

# Sanitizers. SANITIZER is checked in the top-level CMakeLists.txt, which also sets MSVC's
# ASan flags for every target.
set(SANITIZER_FLAGS "")
if(NOT MSVC)
  if(SANITIZER STREQUAL "asan")
    set(SANITIZER_FLAGS -fsanitize=address,undefined -fno-sanitize=vptr)
  elseif(SANITIZER STREQUAL "tsan")
    set(SANITIZER_FLAGS -fsanitize=thread,undefined -fno-sanitize=vptr)
  endif()
  # GCC's sanitizer instrumentation gives false flow-based warnings, which -Werror turns into
  # errors: -Wmaybe-uninitialized in libstdc++'s <regex> and -Wstringop-overflow on a shrinking
  # vector::resize (GCC 14). The first is off there; the others still show but don't fail the
  # build. Plain builds keep all of them as errors.
  if(SANITIZER_FLAGS AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    list(APPEND DEUSEX_WARNINGS -Wno-maybe-uninitialized -Wno-error=stringop-overflow
      -Wno-error=stringop-overread -Wno-error=array-bounds)
  endif()
endif()

# One static library per layer, each linking only the layers below it:
#   dx-core < dx-engine < dx-extension < dx-deusex, with dx-render on dx-engine and dx-compiler
#   on dx-extension.
# Audio is built into dx-engine, because the engine and audio call each other. Each library is
# plain STATIC: the only file-level objects that register anything (the release listeners in
# deusex/menus.cc, pad_inventory.cc and terminals.cc) are linked when the programs use those
# files, as before the split.
#
# A layer's platform code is in its desktop and switch subdirectories, and only the one for
# DX_PLATFORM is built. Shared code reaches it through the layer's platform header
# (core/platform.h, game/platform.h) and has no platform checks of its own.
function(dx_sources var)
  set(sources "")
  foreach(dir ${ARGN})
    file(GLOB found CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${dir}/*.cc ${CMAKE_CURRENT_SOURCE_DIR}/${dir}/${DX_PLATFORM}/*.cc)
    list(APPEND sources ${found})
  endforeach()
  set(${var} ${sources} PARENT_SCOPE)
endfunction()

dx_sources(DX_CORE_SOURCES core)
# The profiling build's global operator new and delete go into each program instead (below), as
# a static library is no sure way to replace them.
list(REMOVE_ITEM DX_CORE_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/core/profile_memory.cc)
include(${CMAKE_SOURCE_DIR}/cmake/embed.cmake)

# Gives a program its icon from assets/icon/<name>: the window's (core/desktop/window_icon.cc), at
# 100 percent scale and the alternates SDL picks from at higher scales, embedded in the program
# alone; and on Windows the .ico as a resource (src/<name>.rc), for Explorer and the taskbar. The
# Switch's is the .nro's, below.
function(dx_program_icon target name)
  if(NINTENDO_SWITCH)
    return()
  endif()
  set(icon ${CMAKE_SOURCE_DIR}/assets/icon/${name})
  embed_files(${CMAKE_CURRENT_BINARY_DIR}/${name}_window_icon.cc windowIcon ${icon}/32.png ${icon}/64.png ${icon}/128.png ${icon}/256.png)
  target_sources(${target} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/${name}_window_icon.cc)
  if(WIN32)
    target_sources(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/${name}.rc)
  endif()
endfunction()

set(AMBROSIA_VERSION "dev" CACHE STRING "Version displayed in the game and logs")
set(AMBROSIA_COMMIT "" CACHE STRING "Source revision for packaged builds")
if(NOT AMBROSIA_VERSION MATCHES "^[A-Za-z0-9][A-Za-z0-9.+-]*$" OR NOT AMBROSIA_COMMIT MATCHES "^[0-9a-f]*$")
  message(FATAL_ERROR "Invalid Ambrosia version or commit")
endif()
set(AMBROSIA_BUILD_LABEL "Ambrosia ${AMBROSIA_VERSION}")
if(AMBROSIA_COMMIT)
  string(APPEND AMBROSIA_BUILD_LABEL " (${AMBROSIA_COMMIT})")
endif()
configure_file(core/version.cc.in version.cc @ONLY)
add_library(dx-core STATIC ${DX_CORE_SOURCES} ${CMAKE_CURRENT_BINARY_DIR}/version.cc)
target_include_directories(dx-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# cli11 public, for the programs that add their own options to the shared ones (core/command_line.h).
target_link_libraries(dx-core PUBLIC SDL3::SDL3-static cli11)
# 1 in profiling builds and 0 otherwise, never undefined, as DX_STEAM_AUDIO.
target_compile_definitions(dx-core PUBLIC DX_PROFILE=$<BOOL:${AMBROSIA_TRACY}>)
if(AMBROSIA_TRACY)
  target_link_libraries(dx-core PUBLIC Tracy::client)
endif()

dx_sources(DX_ENGINE_SOURCES engine audio)

add_library(dx-engine STATIC ${DX_ENGINE_SOURCES})
target_link_libraries(dx-engine PUBLIC dx-core miniaudio)
# 1 with Steam Audio and 0 without, never undefined, so -Wundef still catches a misspelt check.
target_compile_definitions(dx-engine PUBLIC DX_STEAM_AUDIO=$<BOOL:${AMBROSIA_STEAM_AUDIO}>)
if(AMBROSIA_STEAM_AUDIO)
  # Headers only: the library is loaded at run time, and copied next to each program below.
  target_link_libraries(dx-engine PRIVATE SteamAudio::phonon)
endif()

file(GLOB DX_EXTENSION_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/extension/*.cc)
add_library(dx-extension STATIC ${DX_EXTENSION_SOURCES})
target_link_libraries(dx-extension PUBLIC dx-engine)

file(GLOB DX_DEUSEX_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/deusex/*.cc)
file(GLOB DX_GAMEPAD_GLYPHS CONFIGURE_DEPENDS
  ${CMAKE_SOURCE_DIR}/assets/gamepad/prompts/*.png
  ${CMAKE_SOURCE_DIR}/assets/gamepad/layout/*.png)
embed_files(${CMAKE_CURRENT_BINARY_DIR}/gamepad_glyphs.cc gamepadGlyphs ${DX_GAMEPAD_GLYPHS})
embed_files(${CMAKE_CURRENT_BINARY_DIR}/known_logins.cc knownLogins ${CMAKE_SOURCE_DIR}/assets/deusex/known_logins.tsv)
add_library(dx-deusex STATIC ${DX_DEUSEX_SOURCES} ${CMAKE_CURRENT_BINARY_DIR}/gamepad_glyphs.cc ${CMAKE_CURRENT_BINARY_DIR}/known_logins.cc)
# The menu backdrops are the same pixels on every platform only if no multiply and add is fused
# into one rounding (GCC fuses them on ARM by default); MSVC fuses none unless asked.
if(NOT MSVC)
  set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/deusex/menu_backdrop.cc PROPERTIES COMPILE_OPTIONS -ffp-contract=off)
endif()
target_link_libraries(dx-deusex PUBLIC dx-extension)

file(GLOB DX_RENDER_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/render/*.cc)
add_library(dx-render STATIC ${DX_RENDER_SOURCES})
target_include_directories(dx-render PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(dx-render PUBLIC dx-engine glad stb_image)

# Dear ImGui as the editor and the game's developer overlay set it up, and that overlay.
file(GLOB DX_DEBUGUI_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/debugui/*.cc)
add_library(dx-debugui STATIC ${DX_DEBUGUI_SOURCES})
target_link_libraries(dx-debugui PUBLIC dx-render imgui glad)

dx_sources(DX_GAME_SOURCES game)
add_executable(ambrosia ${DX_GAME_SOURCES})
dx_program_icon(ambrosia ambrosia)
# A Windows GUI program, so no console window opens with it. It logs to Ambrosia.log in the user
# directory, and to the console it was started from, if any (core/log.h).
set_target_properties(ambrosia PROPERTIES WIN32_EXECUTABLE TRUE)
target_link_libraries(ambrosia PRIVATE dx-render dx-deusex dx-debugui SDL3::SDL3-static)
if(NINTENDO_SWITCH AND AMBROSIA_SWITCH_MESA)
  target_link_libraries(ambrosia PRIVATE OpenGL::EGL)
endif()

set(DX_TARGETS dx-core dx-engine dx-extension dx-deusex dx-render dx-debugui ambrosia)
set(DX_PROGRAMS ambrosia)

# The Switch build is the game, as build/switch/ambrosia.nro, and the runner that starts it for a PC
# on the network, build/switch/ambrosia-runner.nro (switch-runner/main.cc, which uses libnx alone).
if(NINTENDO_SWITCH)
  nx_generate_nacp(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/ambrosia.nacp NAME "Ambrosia" AUTHOR "Ambrosia" VERSION "0.1.0")
  nx_create_nro(ambrosia OUTPUT ${CMAKE_BINARY_DIR}/ambrosia.nro NACP ${CMAKE_CURRENT_BINARY_DIR}/ambrosia.nacp ICON ${CMAKE_SOURCE_DIR}/assets/icon/ambrosia/switch.jpg)

  add_executable(ambrosia-runner ${CMAKE_CURRENT_SOURCE_DIR}/switch-runner/main.cc)
  target_compile_options(ambrosia-runner PRIVATE ${DEUSEX_WARNINGS})
  nx_generate_nacp(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/ambrosia-runner.nacp NAME "Ambrosia runner" AUTHOR "Ambrosia" VERSION "1")
  nx_create_nro(ambrosia-runner OUTPUT ${CMAKE_BINARY_DIR}/ambrosia-runner.nro NACP ${CMAKE_CURRENT_BINARY_DIR}/ambrosia-runner.nacp)
endif()

if(NOT NINTENDO_SWITCH)
  file(GLOB DX_COMPILER_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/compiler/*.cc)
  add_library(dx-compiler STATIC ${DX_COMPILER_SOURCES})
  target_include_directories(dx-compiler PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(dx-compiler PUBLIC dx-extension)
  list(APPEND DX_TARGETS dx-compiler)

  file(GLOB DX_UCC_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/ucc/*.cc)
  add_executable(ucc ${DX_UCC_SOURCES})
  target_link_libraries(ucc PRIVATE dx-compiler dx-deusex SDL3::SDL3-static)
  list(APPEND DX_TARGETS ucc)
  list(APPEND DX_PROGRAMS ucc)
endif()

file(GLOB DX_EDITOR_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/editor/*.cc)
if(DX_EDITOR_SOURCES AND NOT NINTENDO_SWITCH)
  add_executable(dented ${DX_EDITOR_SOURCES})
  dx_program_icon(dented dented)
  # As the game: no console window; it logs to DentEd.log in the user directory.
  set_target_properties(dented PROPERTIES WIN32_EXECUTABLE TRUE)
  target_link_libraries(dented PRIVATE dx-render dx-compiler dx-deusex dx-debugui imgui imguizmo SDL3::SDL3-static)
  target_include_directories(dented SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/vendor/icons)
  list(APPEND DX_TARGETS dented)
  list(APPEND DX_PROGRAMS dented)
endif()

# Also called from tests/unit, so the flags it reads are passed up to the parent scope.
function(deusex_target_options tgt)
  target_compile_options(${tgt} PRIVATE ${DEUSEX_WARNINGS})

  if(SANITIZER_FLAGS)
    target_compile_options(${tgt} PRIVATE ${SANITIZER_FLAGS} -fno-omit-frame-pointer -g)
    target_link_options(${tgt} PRIVATE ${SANITIZER_FLAGS})
  endif()

  target_compile_definitions(${tgt} PRIVATE $<$<CONFIG:Debug>:_GLIBCXX_ASSERTIONS>)

  if(MSVC)
    target_compile_definitions(${tgt} PRIVATE _CRT_SECURE_NO_WARNINGS)
  endif()

  # Script recursion is stopped at 250 calls, which needs more than the 1 MB Windows default stack.
  get_target_property(type ${tgt} TYPE)
  if(type STREQUAL "EXECUTABLE")
    if(MSVC)
      target_link_options(${tgt} PRIVATE /STACK:16777216)
    elseif(WIN32)
      target_link_options(${tgt} PRIVATE -Wl,--stack,16777216)
    endif()
    # MSVC's ASan runtime, set in the top-level CMakeLists.txt.
    if(DEUSEX_ASAN_RUNTIME)
      add_custom_command(TARGET ${tgt} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${DEUSEX_ASAN_RUNTIME} $<TARGET_FILE_DIR:${tgt}>
        VERBATIM)
    endif()
    # The profiling build's allocation hooks (cmake/tracy.cmake).
    if(AMBROSIA_TRACY)
      target_sources(${tgt} PRIVATE ${CMAKE_SOURCE_DIR}/src/core/profile_memory.cc)
    endif()
    # Steam Audio, from cmake/steamaudio.cmake. Every program links the audio code through
    # dx-engine.
    if(DEUSEX_STEAMAUDIO_RUNTIME)
      add_custom_command(TARGET ${tgt} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${DEUSEX_STEAMAUDIO_RUNTIME} $<TARGET_FILE_DIR:${tgt}>
        VERBATIM)
    endif()
  endif()
endfunction()
set(DEUSEX_WARNINGS ${DEUSEX_WARNINGS} PARENT_SCOPE)
set(SANITIZER_FLAGS ${SANITIZER_FLAGS} PARENT_SCOPE)

foreach(tgt ${DX_TARGETS})
  deusex_target_options(${tgt})
endforeach()

# Every licence in one file (cmake/licenses.cmake), next to each program. The tests are not
# shipped and go without.
foreach(program ${DX_PROGRAMS})
  add_custom_command(TARGET ${program} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${DEUSEX_LICENSES_FILE} $<TARGET_FILE_DIR:${program}>
    VERBATIM)
endforeach()

# The programs carry their own C++ runtime, so they run without the compiler's libraries. MSVC
# gets the same from CMAKE_MSVC_RUNTIME_LIBRARY in the top-level CMakeLists.txt.
if(NOT MSVC)
  foreach(program ${DX_PROGRAMS})
    target_link_options(${program} PRIVATE -static-libstdc++ -static-libgcc)
  endforeach()
endif()
