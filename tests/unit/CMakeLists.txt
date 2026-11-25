file(GLOB UNIT_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cc)
add_executable(unit-tests ${UNIT_TEST_SOURCES} ${CMAKE_SOURCE_DIR}/src/editor/script_workspace.cc ${CMAKE_SOURCE_DIR}/src/editor/log_capture.cc ${CMAKE_SOURCE_DIR}/src/game/gamepad_input.cc ${CMAKE_SOURCE_DIR}/src/game/${DX_PLATFORM}/switch_gyro.cc ${CMAKE_SOURCE_DIR}/src/game/touch_cursor.cc ${CMAKE_SOURCE_DIR}/src/game/menu_options.cc ${CMAKE_SOURCE_DIR}/src/game/pad_prompts.cc)
target_link_libraries(unit-tests PRIVATE dx-render dx-compiler dx-deusex dx-debugui Catch2 SDL3::SDL3-static)
add_dependencies(unit-tests ucc)
deusex_target_options(unit-tests)

# One ctest entry per test file, so a crash or hang is pinned to a file. -# tags every test
# with [#<file>]. Hidden tests ([.editor-scripts]) are left to their own entry.
# The tests tagged [data] fail, saying the game data is missing, when they cannot find it at run
# time (tests/unit/test_paths.h); AMBROSIA_DATA_TESTS=OFF leaves them out where there is none.
if(AMBROSIA_DATA_TESTS)
    set(UNIT_TEST_FILTER "~[.]")
else()
    message(STATUS "AMBROSIA_DATA_TESTS is off, tests tagged [data] are not registered")
    set(UNIT_TEST_FILTER "~[.]~[data]")
endif()
# Timeouts in seconds; a Release run takes 80 s for compiler_test and a few seconds for the rest.
if(SANITIZER STREQUAL "none")
    set(UNIT_TEST_TIMEOUT 300)
else()
    set(UNIT_TEST_TIMEOUT 1200)
endif()
set(UNIT_TESTS "")
foreach(source ${UNIT_TEST_SOURCES})
    file(STRINGS ${source} cases REGEX "(TEST_CASE|SCENARIO)")
    if(cases)
        get_filename_component(stem ${source} NAME_WE)
        # A file whose tests all need the game data runs none without it.
        add_test(NAME unit-${stem} COMMAND unit-tests "-#" --allow-running-no-tests "[#${stem}]${UNIT_TEST_FILTER}")
        list(APPEND UNIT_TESTS unit-${stem})
    endif()
endforeach()
set_tests_properties(${UNIT_TESTS} PROPERTIES TIMEOUT ${UNIT_TEST_TIMEOUT})

if(AMBROSIA_DATA_TESTS)
    add_test(NAME editor-scripts COMMAND unit-tests "[editor-scripts]")
    set_tests_properties(editor-scripts PROPERTIES TIMEOUT ${UNIT_TEST_TIMEOUT})
    list(APPEND UNIT_TESTS editor-scripts)
endif()
# The licences the build ships (cmake/licenses.cmake), checked by section heading.
set(DX_LICENSE_SECTIONS "Ambrosia (GNU AGPL, version 3 or later)" "OpenDE by jarmitage99, the code Ambrosia started from (MIT)" "SDL 3" "miniaudio" "Dear ImGui" "ImPlot" "ImGuizmo" "Input Prompts by Kenney" "Controller illustrations by Zacksly")
if(AMBROSIA_STEAM_AUDIO)
    list(APPEND DX_LICENSE_SECTIONS "Steam Audio")
endif()
add_test(NAME licenses COMMAND ${CMAKE_COMMAND} "-DLICENSES=$<TARGET_FILE_DIR:ambrosia>/LICENSES.txt" "-DSECTIONS=${DX_LICENSE_SECTIONS}" -P ${CMAKE_SOURCE_DIR}/tests/check_licenses.cmake)

# SDL's offscreen driver needs EGL, which Windows lacks, so tests use a hidden window there.
# With ASan, lsan.supp leaves out what Mesa's EGL setup keeps; the tests that open a GL context
# also take full malloc stacks, which reach SDL's frame through the unloaded libEGL.
set(OFFSCREEN_TEST_ENV SDL_VIDEODRIVER=offscreen)
set(GL_TEST_ENV SDL_VIDEODRIVER=offscreen)
if(SANITIZER STREQUAL "asan" AND NOT MSVC)
    list(APPEND OFFSCREEN_TEST_ENV LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/lsan.supp)
    list(APPEND GL_TEST_ENV LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/lsan.supp ASAN_OPTIONS=fast_unwind_on_malloc=0)
endif()
# With TSan, tsan.supp leaves out what Mesa's and Steam Audio's own threads do inside them.
if(SANITIZER STREQUAL "tsan")
    list(APPEND OFFSCREEN_TEST_ENV TSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tsan.supp)
    list(APPEND GL_TEST_ENV TSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tsan.supp)
endif()
if(NOT WIN32)
    set_tests_properties(${UNIT_TESTS} PROPERTIES ENVIRONMENT "${OFFSCREEN_TEST_ENV}")
    # The tests that open a GL context (SDL_GL_CreateContext).
    foreach(gl_test unit-scene_pass_test unit-ambient_occlusion_test unit-mesh_modulated_test unit-gc_render_test unit-dynamic_lights_test)
        if(TEST ${gl_test})
            set_tests_properties(${gl_test} PROPERTIES ENVIRONMENT "${GL_TEST_ENV}")
        endif()
    endforeach()
endif()

if(AMBROSIA_DATA_TESTS AND TARGET dented)
    function(add_editor_test name expected)
        if(WIN32)
            add_test(NAME editor-${name} COMMAND dented --new EditorTest --width 1280 --height 800 --hidden --script ${CMAKE_SOURCE_DIR}/tests/editor/${name}.txt)
        else()
            add_test(NAME editor-${name} COMMAND dented --new EditorTest --width 1280 --height 800 --script ${CMAKE_SOURCE_DIR}/tests/editor/${name}.txt)
            set_tests_properties(editor-${name} PROPERTIES ENVIRONMENT "${GL_TEST_ENV}")
        endif()
        set_tests_properties(editor-${name} PROPERTIES PASS_REGULAR_EXPRESSION "expectations: ${expected} passed, 0 failed" TIMEOUT 120)
    endfunction()
    # Each test's steps are in tests/editor/<name>.txt; the number is how many of its expects pass.
    add_editor_test(escape-ignored-while-popup-open 2)
    add_editor_test(extrude-drag-snaps 4)
    add_editor_test(face-extrude 5)
    add_editor_test(overlapping-handles-cycle 3)
    add_editor_test(scale-gizmo-scales-every-selected-brush 2)
    add_editor_test(vertex-drag-snaps-absolute 5)
endif()
