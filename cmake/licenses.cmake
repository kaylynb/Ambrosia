# LICENSES.txt: Ambrosia's licence and those of everything the programs carry, one after another in
# a single file, written at configure time into the build directory. Every input is in the
# checkout or downloaded at configure time (cmake/steamaudio.cmake, cmake/tracy.cmake), so a
# change to any of them configures again. src/CMakeLists.txt copies the file next to each
# program.
#
# Sets DEUSEX_LICENSES_FILE, the file written.

set(DEUSEX_LICENSES_FILE "${CMAKE_BINARY_DIR}/LICENSES.txt")

set(dx_licenses_rule "================================================================================")
set(dx_licenses_text "Ambrosia and the third-party software and art it includes, with their licences.\n")

# A section: the component, where it comes from, what in Ambrosia uses it, and its licence files in
# order.
function(dx_license_section name url used)
    set(text "${dx_licenses_text}\n\n${dx_licenses_rule}\n${name}\n")
    if(url)
        string(APPEND text "${url}\n")
    endif()
    string(APPEND text "Used by: ${used}\n${dx_licenses_rule}\n")
    set(first TRUE)
    foreach(file ${ARGN})
        if(NOT EXISTS "${file}")
            message(FATAL_ERROR "Licence file ${file} for ${name} not found")
        endif()
        file(READ "${file}" contents)
        # Windows line endings and trailing blank lines are left out so the sections line up.
        string(REPLACE "\r\n" "\n" contents "${contents}")
        string(REGEX REPLACE "[ \t\n]+$" "" contents "${contents}")
        if(NOT first)
            string(APPEND text "\n\n--------------------------------------------------------------------------------\n")
        endif()
        string(APPEND text "\n${contents}\n")
        set(first FALSE)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${file}")
    endforeach()
    set(dx_licenses_text "${text}" PARENT_SCOPE)
endfunction()

set(dx_vendor "${CMAKE_SOURCE_DIR}/vendor")
set(dx_gamepad "${CMAKE_SOURCE_DIR}/assets/gamepad")

# Ambrosia is under the AGPL, version 3 or later. The OpenDE code it started from keeps its MIT
# notice, which has to go with every copy.
dx_license_section("Ambrosia (GNU AGPL, version 3 or later)" "" "everything"
    "${CMAKE_SOURCE_DIR}/LICENSE")
dx_license_section("OpenDE by jarmitage99, the code Ambrosia started from (MIT)" "https://github.com/jarmitage99/OpenDE" "everything"
    "${CMAKE_SOURCE_DIR}/LICENSE-MIT")
if(NOT NINTENDO_SWITCH)
    dx_license_section("SDL 3" "https://www.libsdl.org" "every program"
        "${dx_vendor}/SDL3-3.4.20/LICENSE.txt")
else()
    # devkitPro's SDL3 fork, under SDL's licence, which the vendored copy carries too.
    dx_license_section("SDL 3 (devkitPro's Switch port)" "https://github.com/devkitPro/SDL" "the game"
        "${dx_vendor}/SDL3-3.4.20/LICENSE.txt")
endif()
dx_license_section("glad (OpenGL loader) and the Khronos headers" "https://github.com/Dav1dde/glad" "the game and the editor"
    "${dx_vendor}/glad-gl33/LICENSE")
dx_license_section("miniaudio" "https://miniaud.io" "every program"
    "${dx_vendor}/miniaudio/LICENSE")
dx_license_section("stb_image" "https://github.com/nothings/stb" "the game and the editor"
    "${dx_vendor}/stb/LICENSE")
dx_license_section("Dear ImGui" "https://github.com/ocornut/imgui" "the editor and the game's developer overlay"
    "${dx_vendor}/imgui/LICENSE.txt")
dx_license_section("ImPlot" "https://github.com/epezent/implot" "the editor and the game's developer overlay"
    "${dx_vendor}/implot/LICENSE")
dx_license_section("ProggyClean and ProggyForever fonts (embedded in Dear ImGui)" "https://github.com/bluescan/proggyfonts" "the editor and the game's developer overlay"
    "${dx_vendor}/imgui/LICENSE-fonts.txt")
if(NOT NINTENDO_SWITCH)
    dx_license_section("ImGuizmo" "https://github.com/CedricGuillemet/ImGuizmo" "the editor"
        "${dx_vendor}/imguizmo/LICENSE")
    dx_license_section("Tabler Icons (a subset)" "https://tabler.io/icons" "the editor"
        "${dx_vendor}/icons/LICENSE")
endif()
dx_license_section("Input Prompts by Kenney" "https://www.kenney.nl" "the game's controller button prompts"
    "${dx_gamepad}/prompts/LICENSE-kenney.txt")
dx_license_section("Controller illustrations by Zacksly" "https://zacksly.itch.io/" "the game's Controller Layout screen"
    "${dx_gamepad}/layout/CREDITS.txt"
    "${dx_gamepad}/layout/LICENSE-xbox.txt"
    "${dx_gamepad}/layout/LICENSE-ps.txt"
    "${dx_gamepad}/layout/LICENSE-switch.txt")
if(DEUSEX_STEAMAUDIO_LICENSES)
    dx_license_section("Steam Audio" "https://valvesoftware.github.io/steam-audio/" "the HRTF sound mode (phonon.dll or libphonon.so)"
        ${DEUSEX_STEAMAUDIO_LICENSES})
endif()
if(DEUSEX_TRACY_LICENSE)
    dx_license_section("Tracy Profiler (client)" "https://github.com/wolfpld/tracy" "profiling builds"
        "${DEUSEX_TRACY_LICENSE}")
endif()

if(NINTENDO_SWITCH AND AMBROSIA_SWITCH_MESA)
    # container/Containerfile.switch-mesa puts Mesa's licence there.
    dx_license_section("Mesa (mesa-switch)" "https://www.mesa3d.org" "the game, built with AMBROSIA_SWITCH_MESA"
        "${DEVKITPRO}/portlibs/switch/share/licenses/mesa-switch/license.rst")
endif()

# Written only when it changes, so the copies next to the programs are not redone for nothing.
set(dx_licenses_old "")
if(EXISTS "${DEUSEX_LICENSES_FILE}")
    file(READ "${DEUSEX_LICENSES_FILE}" dx_licenses_old)
endif()
if(NOT dx_licenses_old STREQUAL dx_licenses_text)
    file(WRITE "${DEUSEX_LICENSES_FILE}" "${dx_licenses_text}")
endif()
