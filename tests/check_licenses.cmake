# ctest: LICENSES.txt is next to the game and has a section for each component the build carries.
#   cmake -DLICENSES=<file> -DSECTIONS=<name;name;...> -P check_licenses.cmake
if(NOT EXISTS "${LICENSES}")
    message(FATAL_ERROR "${LICENSES} is missing")
endif()
file(READ "${LICENSES}" text)
foreach(section ${SECTIONS})
    string(FIND "${text}" "\n${section}\n" at)
    if(at EQUAL -1)
        message(FATAL_ERROR "${LICENSES} has no ${section} section")
    endif()
endforeach()
message(STATUS "LICENSES.txt has all ${SECTIONS}")
