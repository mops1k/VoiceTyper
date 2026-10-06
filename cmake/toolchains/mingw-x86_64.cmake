set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

if(DEFINED ENV{VOICETYPER_MINGW_ROOT})
    file(TO_CMAKE_PATH "$ENV{VOICETYPER_MINGW_ROOT}" _voicetyper_mingw_root)
elseif(DEFINED ENV{MINGW_ROOT})
    file(TO_CMAKE_PATH "$ENV{MINGW_ROOT}" _voicetyper_mingw_root)
else()
    message(FATAL_ERROR "Set VOICETYPER_MINGW_ROOT or MINGW_ROOT to a verified MinGW-w64 root")
endif()

foreach(_voicetyper_tool gcc g++ windres)
    if(NOT EXISTS "${_voicetyper_mingw_root}/bin/${_voicetyper_tool}.exe")
        message(FATAL_ERROR "MinGW tool not found: ${_voicetyper_mingw_root}/bin/${_voicetyper_tool}.exe")
    endif()
endforeach()

set(CMAKE_C_COMPILER "${_voicetyper_mingw_root}/bin/gcc.exe" CACHE FILEPATH "C compiler" FORCE)
set(CMAKE_CXX_COMPILER "${_voicetyper_mingw_root}/bin/g++.exe" CACHE FILEPATH "C++ compiler" FORCE)
set(CMAKE_RC_COMPILER "${_voicetyper_mingw_root}/bin/windres.exe" CACHE FILEPATH "resource compiler" FORCE)

if(DEFINED ENV{VOICETYPER_QT_ROOT})
    file(TO_CMAKE_PATH "$ENV{VOICETYPER_QT_ROOT}" _voicetyper_qt_root)
elseif(DEFINED Qt6_DIR)
    get_filename_component(_qt6_dir "${Qt6_DIR}" ABSOLUTE)
    get_filename_component(_voicetyper_qt_root "${_qt6_dir}/../../.." ABSOLUTE)
else()
    message(FATAL_ERROR "Set VOICETYPER_QT_ROOT or pass -DQt6_DIR=<path-to-Qt6Config.cmake>")
endif()

if(NOT EXISTS "${_voicetyper_qt_root}/lib/cmake/Qt6/Qt6Config.cmake")
    message(FATAL_ERROR "Qt6Config.cmake not found under ${_voicetyper_qt_root}/lib/cmake/Qt6")
endif()

set(CMAKE_PREFIX_PATH "${_voicetyper_qt_root}" CACHE PATH "Qt SDK root" FORCE)
set(CMAKE_FIND_ROOT_PATH "${_voicetyper_qt_root};${_voicetyper_mingw_root}" CACHE STRING "Qt and MinGW roots" FORCE)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

execute_process(
    COMMAND "${CMAKE_CXX_COMPILER}" -dumpversion
    OUTPUT_VARIABLE _voicetyper_compiler_version
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
message(STATUS "VoiceTyper MinGW compiler: ${CMAKE_CXX_COMPILER} (${_voicetyper_compiler_version})")
