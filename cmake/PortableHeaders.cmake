# Portable-header check.
#
# src/domain and src/platform/api are contract-only layers: they must compile
# with nothing but the standard C++20 library, so that
#   * a GUI-off build stays Qt-free,
#   * a Windows backend and a Linux backend can both be written behind the same
#     interfaces, and
#   * OS SDK / third-party leakage is caught at configure time rather than when
#     someone tries to build for the other platform.
#
# Two entry points, same implementation:
#   Configure time:  include(cmake/PortableHeaders.cmake)
#                   voicetyper_check_portable_headers("${PROJECT_SOURCE_DIR}")
#   Test time:       cmake -DVOICETYPER_SOURCE_DIR=<src> -P cmake/PortableHeaders.cmake
#
# Only `#include` directives are inspected, so a comment that merely names a
# header never fails the build.

if(CMAKE_SCRIPT_MODE_FILE)
    set(VOICETYPER_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/.." CACHE PATH "VoiceTyper source root")
    get_filename_component(VOICETYPER_SOURCE_DIR "${VOICETYPER_SOURCE_DIR}" ABSOLUTE)
endif()

# Lower-case fragments that must not appear in an include directive.
# Matching is case-insensitive because the content is lower-cased first.
set(VOICETYPER_FORBIDDEN_INCLUDE_PATTERNS
    # Windows SDK / Win32
    "windows\\.h"
    "windef\\.h"
    "winnt\\.h"
    "winbase\\.h"
    "winuser\\.h"
    "user32\\.h"
    "gdi32\\.h"
    "ole2?\\.h"
    "combaseapi\\.h"
    "unknwn\\.h"
    "objbase\\.h"
    "shobjidl"
    "shellapi\\.h"
    "shlobj"
    "winreg\\.h"
    "tlhelp32\\.h"
    "cfgmgr32\\.h"
    "setupapi\\.h"
    "winsock2?\\.h"
    "ws2tcpip\\.h"
    "process\\.h"
    "powrprof\\.h"
    # Windows audio / input
    "audioclient\\.h"
    "mmdeviceapi\\.h"
    "xaudio2"
    "mmsystem\\.h"
    "timeapi\\.h"
    "avrt\\.h"
    "ks\\.h"
    "ksmedia\\.h"
    "ksuser\\.h"
    "dinput\\.h"
    "xinput\\.h"
    "mmio\\.h"
    # COM / dynamic loading
    "atlbase\\.h"
    "atlconv\\.h"
    "dlfcn\\.h"
    # X11 / Wayland / ALSA / PulseAudio
    "x11/"
    "xlib\\.h"
    "xkbcommon"
    "wayland-client"
    "alsa/"
    "asoundlib\\.h"
    "pulse/"
    "pulseaudio"
    # Qt (any spelling of the module directories or the main classes)
    "qtcore"
    "qtgui"
    "qtwidgets"
    "qtnetwork"
    "qtsvg"
    "qglobal\\.h"
    "qapplication"
    "qwidget"
    "qobject"
    "qstring"
    "qcoreapplication"
    # Third-party libraries the C++ port must wrap, not include
    "nauudio"
    "whisper\\.h"
    "whisper-vad"
    "parakeet_capi"
    "nlohmann"
    "stb_image"
)

function(voicetyper_check_portable_headers source_dir)
    if(NOT IS_DIRECTORY "${source_dir}/src/domain")
        message(FATAL_ERROR "portable-header check: ${source_dir}/src/domain does not exist")
    endif()

    file(GLOB_RECURSE headers
        "${source_dir}/src/domain/*.hpp"
        "${source_dir}/src/platform/api/*.hpp"
    )
    list(LENGTH headers header_count)

    set(violations "")
    foreach(header IN LISTS headers)
        file(READ "${header}" content)
        string(TOLOWER "${content}" lowered)
        string(REGEX MATCHALL "#[ \t]*include[ \t]*[<\"][^>\"]*[>\"]" includes "${lowered}")
        foreach(directive IN LISTS includes)
            foreach(pattern IN LISTS VOICETYPER_FORBIDDEN_INCLUDE_PATTERNS)
                if(directive MATCHES "${pattern}")
                    file(RELATIVE_PATH relative "${source_dir}" "${header}")
                    list(APPEND violations "${relative}: ${directive} (forbidden pattern '${pattern}')")
                endif()
            endforeach()
        endforeach()
    endforeach()

    list(LENGTH violations violation_count)
    if(violation_count GREATER 0)
        string(REPLACE ";" "\n  " readable "${violations}")
        message(FATAL_ERROR
            "portable-header check FAILED: ${violation_count} forbidden include(s) in contract headers.\n"
            "  ${readable}\n"
            "src/domain and src/platform/api must include standard C++20 headers only.\n"
            "Move the OS call behind a src/platform/api interface instead.")
    endif()

    # Printed at configure time and re-printed when the script runs under ctest,
    # so both invocations produce the same greppable success line.
    message(STATUS "portable-header check: OK (${header_count} contract headers, standard C++ only)")
    return()
endfunction()

if(CMAKE_SCRIPT_MODE_FILE)
    voicetyper_check_portable_headers("${VOICETYPER_SOURCE_DIR}")
endif()
