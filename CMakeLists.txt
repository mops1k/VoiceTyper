cmake_minimum_required(VERSION 3.28)

project(VoiceTyperCpp VERSION 0.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(VOICETYPER_BUILD_GUI "Build the Qt Widgets shell" ON)
option(VOICETYPER_BUILD_TESTS "Build CTest smoke tests" ON)
# The pinned native ASR dependency (whisper.cpp) is fetched from the network, so
# its default follows the application build: a GUI-off portable contract build
# stays completely offline, and any build that produces a binary links the
# engine it is supposed to contain. Pass -DVOICETYPER_BUILD_ASR=ON to prove the
# native engine builds without a GUI, or -DVOICETYPER_BUILD_ASR=OFF to force a
# fully offline configuration.
option(VOICETYPER_BUILD_ASR "Build the pinned whisper.cpp dependency (requires network once)" ${VOICETYPER_BUILD_GUI})

# The product version. It is what the About page shows and what the update check
# compares against the release tag, so it must be a real version, not the spike
# default. CI overrides it with the tag being released.
set(VOICETYPER_VERSION "2.0.0" CACHE STRING "Product version, compared with the release tag")

# Native dependency contract (task t_c6a6b0ce9685). The manifest is read on
# every configuration, the Parakeet pin/ABI are cross-checked against the
# portable registry header, and the shipped DLLs are hash-verified. All of that
# is offline; only the whisper.cpp FetchContent inside
# voicetyper_fetch_whisper_cpp() touches the network.
include(${PROJECT_SOURCE_DIR}/cmake/NativeAsr.cmake)
voicetyper_verify_native_pins()
voicetyper_verify_shipped_dll(parakeet.cpp "native/parakeet.dll")
set(VOICETYPER_PARAKEET_DLL_VERIFIED "${verified}")
voicetyper_verify_shipped_dll(mc_wasapi "native/mc_wasapi.dll")
set(VOICETYPER_MCWASAPI_DLL_VERIFIED "${verified}")
voicetyper_native_manifest_artifact(parakeet.cpp "native/parakeet.dll" sha256 VOICETYPER_PARAKEET_DLL_SHA256)
voicetyper_native_manifest_artifact(mc_wasapi "native/mc_wasapi.dll" sha256 VOICETYPER_MCWASAPI_DLL_SHA256)

# Contract headers must stay standard-C++-only. Failing here rather than at link
# time keeps a GUI-off build Qt-free and keeps OS SDK leakage out of the
# portable layer. Re-run under ctest as the "portable-headers" test.
include(${PROJECT_SOURCE_DIR}/cmake/PortableHeaders.cmake)
voicetyper_check_portable_headers("${PROJECT_SOURCE_DIR}")

if(VOICETYPER_BUILD_GUI)
    # Network is required by QLocalServer/QLocalSocket (the single-instance guard).
    # The floor is 6.9 rather than an exact local version: CI installs the newest
    # PUBLISHED Qt (6.9.3, the local 6.11.2 is a private build), and an EXACT requirement
    # made the release build fail with "Could not find a configuration file for package
    # Qt6 that exactly matches". 6.9 is what both toolchains actually build.
    find_package(Qt6 6.9 REQUIRED COMPONENTS Core Gui Widgets Network)
endif()

# Python is optional for C++ compilation but required for fixture verification
# and the differential harness. Do not hide its absence: a local Windows box
# without Python will print this status and omit those two CTest entries.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    message(STATUS "fixture/differential tests: enabled (${Python3_EXECUTABLE})")
else()
    message(STATUS "fixture/differential tests: DISABLED (Python3 interpreter not found)")
endif()

function(voicetyper_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

# Header-only platform contract layer. Deliberately links nothing: no Qt, no OS
# SDK, no third-party library. Every backend (src/platform/windows,
# src/platform/linux) implements these interfaces behind its own target.
add_library(voicetyper_platform INTERFACE)
target_include_directories(voicetyper_platform
    INTERFACE
        ${PROJECT_SOURCE_DIR}/src
)
target_compile_features(voicetyper_platform INTERFACE cxx_std_20)

add_library(voicetyper_domain STATIC
    src/domain/version.cpp
)
target_include_directories(voicetyper_domain
    PUBLIC
        ${PROJECT_SOURCE_DIR}/src
)
target_compile_features(voicetyper_domain PUBLIC cxx_std_20)
target_compile_definitions(voicetyper_domain
    PRIVATE
        VOICETYPER_VERSION="${VOICETYPER_VERSION}"
)
target_link_libraries(voicetyper_domain PUBLIC voicetyper_platform)
voicetyper_set_warnings(voicetyper_domain)

add_library(voicetyper_settings STATIC
    src/domain/settings_json.cpp
)
target_link_libraries(voicetyper_settings PUBLIC voicetyper_domain)
voicetyper_set_warnings(voicetyper_settings)

add_library(voicetyper_core_support STATIC
    src/domain/app_paths.cpp
    src/domain/file_logger.cpp
    src/domain/cpu_topology.cpp
)
target_link_libraries(voicetyper_core_support PUBLIC voicetyper_domain)
voicetyper_set_warnings(voicetyper_core_support)

add_library(voicetyper_audio STATIC
    src/domain/audio_wav.cpp
)
target_link_libraries(voicetyper_audio PUBLIC voicetyper_domain)
voicetyper_set_warnings(voicetyper_audio)

add_library(voicetyper_vad STATIC
    src/domain/vad.cpp
)
target_link_libraries(voicetyper_vad PUBLIC voicetyper_domain)
voicetyper_set_warnings(voicetyper_vad)

add_library(voicetyper_state STATIC
    src/domain/recording_state_machine.cpp
)
target_link_libraries(voicetyper_state PUBLIC voicetyper_domain voicetyper_audio voicetyper_vad voicetyper_output)
voicetyper_set_warnings(voicetyper_state)

# Presentation state for the settings UI: portable, so the 700 ms autosave
# debounce, the dirty tracking and the atomic save are testable without Qt.
add_library(voicetyper_presenter STATIC
    src/app/settings_presenter.cpp
)
target_link_libraries(voicetyper_presenter PUBLIC voicetyper_settings voicetyper_platform)
voicetyper_set_warnings(voicetyper_presenter)

# Clipboard/paste policy over the platform seams. Portable, so the retry, timing,
# cancellation and UAC behaviour are contract-tested without a desktop session.
add_library(voicetyper_output STATIC
    src/domain/text_output.cpp
)
target_link_libraries(voicetyper_output PUBLIC voicetyper_domain voicetyper_platform)
voicetyper_set_warnings(voicetyper_output)

# Engine-independent ASR request mapping: no whisper.cpp, no parakeet, no Qt.
# This is the part of the ASR phase that can be contract-tested anywhere.
add_library(voicetyper_asr STATIC
    src/asr/engine_parameters.cpp
)
target_link_libraries(voicetyper_asr PUBLIC voicetyper_domain voicetyper_audio)
voicetyper_set_warnings(voicetyper_asr)

# Concrete engine registry over the two native engines, with the factories
# injected so the "never substitute another engine" rule is testable everywhere.
add_library(voicetyper_asr_registry STATIC
    src/asr/native_engine_registry.cpp
)
target_link_libraries(voicetyper_asr_registry PUBLIC voicetyper_domain voicetyper_platform)
voicetyper_set_warnings(voicetyper_asr_registry)

# Engine lifecycle owner: epochs, background load, readiness. Portable, so the
# lifecycle contract runs without whisper.cpp, parakeet.dll, Qt or a network.
add_library(voicetyper_asr_lifecycle STATIC
    src/asr/engine_host.cpp
)
target_link_libraries(voicetyper_asr_lifecycle PUBLIC voicetyper_asr voicetyper_state)
voicetyper_set_warnings(voicetyper_asr_lifecycle)

# The microphone release fuse: a registered release runs once when the process dies,
# because a device left allocated by a dead process blocks every application.
add_library(voicetyper_capture_guard STATIC
    src/platform/capture_guard.cpp
)
target_link_libraries(voicetyper_capture_guard PUBLIC voicetyper_domain)
voicetyper_set_warnings(voicetyper_capture_guard)

# The microphone's own input level and mute state (Windows: IAudioEndpointVolume on
# the default capture endpoint). A seam of its own, because the settings page offers
# a sensitivity slider and a microphone test and both need it.
add_library(voicetyper_microphone_level STATIC
    src/platform/microphone_level.cpp
)
target_link_libraries(voicetyper_microphone_level PUBLIC voicetyper_domain)
voicetyper_set_warnings(voicetyper_microphone_level)

# Model catalog and download policy. Pure control flow over the injected
# FileSystem/HttpClient/Clock seams, so it stays testable without a network.
add_library(voicetyper_model_store STATIC
    src/platform/model_catalog.cpp
    src/platform/catalog_model_store.cpp
)
target_link_libraries(voicetyper_model_store PUBLIC voicetyper_domain voicetyper_platform)
voicetyper_set_warnings(voicetyper_model_store)

# Parakeet runtime: a runtime-loaded binding to the shipped parakeet.dll. It is
# built on every platform, because the interesting half of its contract - "the
# engine is explicitly unavailable, with a reason, and no other engine is
# substituted" - is exactly what the other platforms have to prove. The
# LoadLibraryW/GetProcAddress calls are compiled only on Windows.
add_library(voicetyper_parakeet_runtime STATIC
    src/platform/windows/parakeet_runtime.cpp
)
target_link_libraries(voicetyper_parakeet_runtime PUBLIC voicetyper_domain voicetyper_platform)
voicetyper_set_warnings(voicetyper_parakeet_runtime)

if(VOICETYPER_BUILD_ASR)
    # whisper.cpp and ggml are C/C++ projects, so the C language is enabled only
    # when they are actually part of the build.
    enable_language(C)
    voicetyper_fetch_whisper_cpp()

    # The thin C++ wrapper: the only place that includes the real whisper.h.
    add_library(voicetyper_asr_whisper STATIC
        src/asr/whisper_native.cpp
    )
    target_link_libraries(voicetyper_asr_whisper
        PUBLIC
            voicetyper_domain
        PRIVATE
            voicetyper_whisper_native
    )
    voicetyper_set_warnings(voicetyper_asr_whisper)

    # The two real engines behind the frozen platform::Transcriber contract, plus
    # the concrete registry that creates them. Only exists when whisper.cpp is in
    # the build: without it, Whisper is reported platform_unsupported by the
    # registry and the contract test above proves there is no silent fallback.
    add_library(voicetyper_asr_native STATIC
        src/asr/native_transcribers.cpp
        src/asr/native_engine_registry.cpp
    )
    target_link_libraries(voicetyper_asr_native
        PUBLIC
            voicetyper_asr_lifecycle
            voicetyper_asr_registry
        PRIVATE
            voicetyper_asr_whisper
            voicetyper_parakeet_runtime
    )
    voicetyper_set_warnings(voicetyper_asr_native)

    # Real-model evidence: registers only when ASR is on, and skips cleanly when
    # no model file is configured, so CI without models stays green.
    add_executable(voicetyper-asr-native-smoke EXCLUDE_FROM_ALL
        tests/integration/asr_native_smoke.cpp
    )
    target_link_libraries(voicetyper-asr-native-smoke PRIVATE voicetyper_asr_native)

    # Diagnostic, not a contract: it answers "which decoder parameter set
    # actually recognises this audio" by running the same fixture through
    # several named whisper configurations. Opt-in by data, like the smoke test.
    add_executable(voicetyper-asr-whisper-probe EXCLUDE_FROM_ALL
        tests/integration/asr_whisper_probe.cpp
    )
    target_link_libraries(voicetyper-asr-whisper-probe PRIVATE voicetyper_whisper_native voicetyper_audio voicetyper_domain)
    voicetyper_set_warnings(voicetyper-asr-whisper-probe)
    voicetyper_set_warnings(voicetyper-asr-native-smoke)
    add_test(NAME asr-native-smoke COMMAND voicetyper-asr-native-smoke)
    set_tests_properties(asr-native-smoke PROPERTIES
        LABELS "asr;native"
        TIMEOUT 600
        SKIP_RETURN_CODE 0
    )
else()
    message(STATUS
        "native ASR dependency: OFF - no whisper.cpp fetch, this configuration is fully offline. "
        "Enable it with -DVOICETYPER_BUILD_ASR=ON for a native build.")
endif()

add_executable(voicetyper-diagnostics EXCLUDE_FROM_ALL
    tools/voicetyper-diagnostics.cpp
)
target_link_libraries(voicetyper-diagnostics PRIVATE voicetyper_domain)
voicetyper_set_warnings(voicetyper-diagnostics)

# Portable fallbacks for the seams the UI needs on any host, including CI.
add_library(voicetyper_portable_runtime STATIC
    src/platform/portable/portable_runtime.cpp
)
target_link_libraries(voicetyper_portable_runtime PUBLIC voicetyper_domain voicetyper_platform)
voicetyper_set_warnings(voicetyper_portable_runtime)

# Update core (plan p_a95b558c6861, Phase 3): the GitHub release manifest
# (QtCore for QJsonDocument only, in the .cpp) and the run-update.cmd bytes.
# It cannot live in src/domain, which the portable-header audit requires to stay
# Qt-free, and it must not live in src/app either: the wire format is not UI.
# Qt is found only when the GUI is built, so the target follows that switch.
if(VOICETYPER_BUILD_GUI)
    add_library(voicetyper_update STATIC
    src/core/support/update_service.cpp
    src/core/support/sha256.cpp
        src/core/support/update_manifest.cpp
        src/core/support/update_launcher.cpp
    )
    target_link_libraries(voicetyper_update
        PUBLIC
            Qt6::Core
            voicetyper_domain
            voicetyper_platform
    )
    voicetyper_set_warnings(voicetyper_update)
endif()

if(VOICETYPER_BUILD_GUI)
    # The window lives in its own library so the UI test can drive the real
    # widgets instead of a copy of their logic. Q_OBJECT needs moc, so automoc is
    # enabled explicitly.
    add_library(voicetyper_ui STATIC
        src/app/main_window.cpp
        src/app/ui_text.cpp
        src/app/application_font.cpp
        src/app/qt_http_client.cpp
        src/app/toggle_switch.cpp
        src/app/status_overlay.cpp
        src/app/tray_controller.cpp
    )
    target_link_libraries(voicetyper_ui
        PUBLIC
            Qt6::Widgets
            Qt6::Network
            voicetyper_domain
        PRIVATE
            voicetyper_presenter
            voicetyper_portable_runtime
    )
    set_target_properties(voicetyper_ui PROPERTIES AUTOMOC ON)
    voicetyper_set_warnings(voicetyper_ui)

    qt_add_executable(voicetyper-qt-shell WIN32
        src/app/main.cpp
        $<$<BOOL:${WIN32}>:src/app/windows_application.cpp>
    )
    # The product icon travels inside the executable as a Qt resource, so the tray,
    # the window and the taskbar show it without shipping a separate file.
    # The product icon and the bundled Inter family travel inside the executable;
    # keep the resource paths in step with application_font.cpp.
    qt_add_resources(voicetyper-qt-shell "voicetyper_assets"
        PREFIX "/"
        FILES
            assets/voiceTyper.png
            assets/fonts/Inter-Regular.ttf
            assets/fonts/Inter-Medium.ttf
            assets/fonts/Inter-SemiBold.ttf
            assets/fonts/Inter-Bold.ttf
    )
    target_link_libraries(voicetyper-qt-shell
        PRIVATE
            voicetyper_ui
            voicetyper_domain
    )
    if(WIN32 AND VOICETYPER_BUILD_ASR)
        # The Windows composition needs the real engines, the recording machine
        # and the Win32 backends together; on other hosts the portable fallbacks
        # keep the window usable.
        target_link_libraries(voicetyper-qt-shell
            PRIVATE
                voicetyper_asr_native
                voicetyper_microphone_level
                voicetyper_capture_guard
                voicetyper_platform_win32_update_launcher
                voicetyper_update
                voicetyper_state
                voicetyper_output
                # FileLogger backs the Журнал page; without it the composition
                # cannot report anything the user would need to debug a run.
                voicetyper_core_support
                voicetyper_platform_win32
                voicetyper_platform_win32_input
                # The capture library owns WindowsMicrophone and
                # WindowsAudioCapture; without it the composition's
                # constructors and list_devices() do not link.
                voicetyper_audio_capture
        )
    endif()
    voicetyper_set_warnings(voicetyper-qt-shell)
endif()

if(VOICETYPER_BUILD_TESTS)
    enable_testing()

    add_executable(voicetyper-domain-smoke EXCLUDE_FROM_ALL
        tests/unit/domain_smoke.cpp
    )
    target_link_libraries(voicetyper-domain-smoke PRIVATE voicetyper_domain)
    voicetyper_set_warnings(voicetyper-domain-smoke)
    add_test(NAME domain-smoke COMMAND voicetyper-domain-smoke)

    add_executable(voicetyper-settings-json-contract EXCLUDE_FROM_ALL
        tests/contract/settings_json_contract.cpp
    )
    target_link_libraries(voicetyper-settings-json-contract PRIVATE voicetyper_settings)
    target_compile_definitions(voicetyper-settings-json-contract PRIVATE
        VOICETYPER_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    )
    voicetyper_set_warnings(voicetyper-settings-json-contract)
    add_test(NAME settings-json-contract COMMAND voicetyper-settings-json-contract)
    set_tests_properties(settings-json-contract PROPERTIES
        PASS_REGULAR_EXPRESSION "settings-json-contract: OK"
    )

    add_executable(voicetyper-core-support-contract EXCLUDE_FROM_ALL
        tests/contract/core_support_contract.cpp
    )
    target_link_libraries(voicetyper-core-support-contract PRIVATE voicetyper_core_support)
    voicetyper_set_warnings(voicetyper-core-support-contract)
    add_test(NAME core-support-contract COMMAND voicetyper-core-support-contract)
    set_tests_properties(core-support-contract PROPERTIES
        PASS_REGULAR_EXPRESSION "core-support-contract: OK"
    )

    add_executable(voicetyper-audio-dsp-contract EXCLUDE_FROM_ALL
        tests/contract/audio_dsp_contract.cpp
    )
    target_link_libraries(voicetyper-audio-dsp-contract PRIVATE voicetyper_audio)
    target_compile_definitions(voicetyper-audio-dsp-contract PRIVATE
        VOICETYPER_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    )
    voicetyper_set_warnings(voicetyper-audio-dsp-contract)
    add_test(NAME audio-dsp-contract COMMAND voicetyper-audio-dsp-contract)
    set_tests_properties(audio-dsp-contract PROPERTIES
        PASS_REGULAR_EXPRESSION "audio-dsp-contract: OK"
    )

    add_executable(voicetyper-mc-wasapi-contract EXCLUDE_FROM_ALL
        tests/contract/mc_wasapi_contract.cpp
        src/platform/mc_wasapi.cpp
    )
    target_link_libraries(voicetyper-mc-wasapi-contract PRIVATE voicetyper_domain)
    voicetyper_set_warnings(voicetyper-mc-wasapi-contract)
    add_test(NAME mc-wasapi-contract COMMAND voicetyper-mc-wasapi-contract)
    set_tests_properties(mc-wasapi-contract PROPERTIES
        PASS_REGULAR_EXPRESSION "mc-wasapi-contract: OK"
    )

    add_executable(voicetyper-microphone-level-contract EXCLUDE_FROM_ALL
        tests/contract/microphone_level_contract.cpp
    )
    target_link_libraries(voicetyper-microphone-level-contract PRIVATE voicetyper_microphone_level)
    voicetyper_set_warnings(voicetyper-microphone-level-contract)
    add_test(NAME microphone-level-contract COMMAND voicetyper-microphone-level-contract)
    set_tests_properties(microphone-level-contract PROPERTIES
        PASS_REGULAR_EXPRESSION "microphone-level-contract: OK"
    )

    add_executable(voicetyper-capture-guard-contract EXCLUDE_FROM_ALL
        tests/contract/capture_guard_contract.cpp
    )
    target_link_libraries(voicetyper-capture-guard-contract PRIVATE voicetyper_capture_guard)
    voicetyper_set_warnings(voicetyper-capture-guard-contract)
    add_test(NAME capture-guard-contract COMMAND voicetyper-capture-guard-contract)
    set_tests_properties(capture-guard-contract PROPERTIES
        PASS_REGULAR_EXPRESSION "capture-guard-contract: OK"
    )

    add_executable(voicetyper-update-service-contract EXCLUDE_FROM_ALL
        tests/contract/update_service_contract.cpp
    )
    target_link_libraries(voicetyper-update-service-contract PRIVATE voicetyper_update)
    voicetyper_set_warnings(voicetyper-update-service-contract)
    add_test(NAME update-service-contract COMMAND voicetyper-update-service-contract)
    set_tests_properties(update-service-contract PROPERTIES
        PASS_REGULAR_EXPRESSION "update-service-contract: OK"
    )

    add_executable(voicetyper-vad-contract EXCLUDE_FROM_ALL
        tests/contract/vad_contract.cpp
    )
    target_link_libraries(voicetyper-vad-contract PRIVATE voicetyper_vad)
    voicetyper_set_warnings(voicetyper-vad-contract)
    add_test(NAME vad-contract COMMAND voicetyper-vad-contract)
    set_tests_properties(vad-contract PROPERTIES
        PASS_REGULAR_EXPRESSION "vad-contract: OK"
    )

    add_executable(voicetyper-asr-parameters-contract EXCLUDE_FROM_ALL
        tests/contract/asr_parameters_contract.cpp
    )
    target_link_libraries(voicetyper-asr-parameters-contract PRIVATE voicetyper_asr)
    voicetyper_set_warnings(voicetyper-asr-parameters-contract)
    add_test(NAME asr-parameters-contract COMMAND voicetyper-asr-parameters-contract)
    set_tests_properties(asr-parameters-contract PROPERTIES
        PASS_REGULAR_EXPRESSION "asr-parameters-contract: OK"
    )

    if(VOICETYPER_BUILD_GUI)
        # The UI contract drives the real widgets and then reads settings.json back
        # from disk: it is the test that answers "do my settings survive a restart".
        find_package(Qt6 QUIET COMPONENTS Test)
        if(Qt6Test_FOUND)
            qt_add_executable(voicetyper-ui-settings-test EXCLUDE_FROM_ALL
                tests/ui/ui_settings_test.cpp
            )
        # The tests build the window with the shipped typeface (see
        # application_font.cpp): an overflow caused by Inter's metrics has to fail
        # here rather than on Alexander's screen.
        qt_add_resources(voicetyper-ui-settings-test "voicetyper_test_assets"
            PREFIX "/"
            FILES
                assets/fonts/Inter-Regular.ttf
                assets/fonts/Inter-Medium.ttf
                assets/fonts/Inter-SemiBold.ttf
                assets/fonts/Inter-Bold.ttf
        )

            target_link_libraries(voicetyper-ui-settings-test
                PRIVATE
                    Qt6::Test
                    voicetyper_ui
            )
            # qt_add_executable sets WIN32_EXECUTABLE TRUE on Windows, which makes a
            # console test binary with no console at all: QTest and even std::cout
            # output vanish, so a PASS_REGULAR_EXPRESSION can never match. The test
            # is a console program and has to stay one.
            set_target_properties(voicetyper-ui-settings-test PROPERTIES
                AUTOMOC ON
                WIN32_EXECUTABLE FALSE)
            voicetyper_set_warnings(voicetyper-ui-settings-test)
            add_test(NAME ui-settings-test COMMAND voicetyper-ui-settings-test)
            set_tests_properties(ui-settings-test PROPERTIES
                PASS_REGULAR_EXPRESSION "ui-settings-test: result=passed"
                TIMEOUT 120
            )

            # A snapshot tool, not a test: it renders every settings page into a PNG
            # so the layout can be reviewed without a screen capture (grab() shows
            # what the layout produced, while PrintWindow and screen grabs showed a
            # different window or a stale build). Windows and Linux alike.
            qt_add_executable(voicetyper-ui-snapshot EXCLUDE_FROM_ALL
                tools/ui_snapshot.cpp
            )
            target_link_libraries(voicetyper-ui-snapshot
                PRIVATE
                    Qt6::Widgets
                    voicetyper_ui
            )
            set_target_properties(voicetyper-ui-snapshot PROPERTIES
                AUTOMOC ON
                WIN32_EXECUTABLE FALSE)
            # Screenshots must show the shipped typeface, not the host default.
            qt_add_resources(voicetyper-ui-snapshot "voicetyper_snapshot_assets"
                PREFIX "/"
                FILES
                    assets/fonts/Inter-Regular.ttf
                    assets/fonts/Inter-Medium.ttf
                    assets/fonts/Inter-SemiBold.ttf
                    assets/fonts/Inter-Bold.ttf
            )
            voicetyper_set_warnings(voicetyper-ui-snapshot)

            # The overlay is a window with frozen observable behaviour (no
            # taskbar entry, no focus, no input capture, 350 ms pulse, geometry),
            # so it is tested against the real widget rather than a copy of the
            # rules. It forces the offscreen platform itself, because a
            # desktop-session-independent contract must not depend on one.
            qt_add_executable(voicetyper-ui-status-overlay-test EXCLUDE_FROM_ALL
                tests/ui/ui_status_overlay_test.cpp
            )
            target_link_libraries(voicetyper-ui-status-overlay-test
                PRIVATE
                    Qt6::Test
                    voicetyper_ui
            )
            set_target_properties(voicetyper-ui-status-overlay-test PROPERTIES
                AUTOMOC ON
                WIN32_EXECUTABLE FALSE
            )
            voicetyper_set_warnings(voicetyper-ui-status-overlay-test)
            add_test(NAME ui-status-overlay-test COMMAND voicetyper-ui-status-overlay-test)
            set_tests_properties(ui-status-overlay-test PROPERTIES
                PASS_REGULAR_EXPRESSION "ui-status-overlay-test: result=passed"
                TIMEOUT 120
            )
        endif()
    endif()

    add_executable(voicetyper-settings-presenter-contract EXCLUDE_FROM_ALL
        tests/contract/settings_presenter_contract.cpp
    )
    target_link_libraries(voicetyper-settings-presenter-contract PRIVATE voicetyper_presenter)
    voicetyper_set_warnings(voicetyper-settings-presenter-contract)
    add_test(NAME settings-presenter-contract COMMAND voicetyper-settings-presenter-contract)
    set_tests_properties(settings-presenter-contract PROPERTIES
        PASS_REGULAR_EXPRESSION "settings-presenter-contract: OK"
    )

    add_executable(voicetyper-text-output-contract EXCLUDE_FROM_ALL
        tests/contract/text_output_contract.cpp
    )
    target_link_libraries(voicetyper-text-output-contract PRIVATE voicetyper_output)
    voicetyper_set_warnings(voicetyper-text-output-contract)
    add_test(NAME text-output-contract COMMAND voicetyper-text-output-contract)
    set_tests_properties(text-output-contract PROPERTIES
        PASS_REGULAR_EXPRESSION "text-output-contract: OK"
        TIMEOUT 120
    )

    add_executable(voicetyper-asr-lifecycle-contract EXCLUDE_FROM_ALL
        tests/contract/asr_lifecycle_contract.cpp
    )
    target_link_libraries(voicetyper-asr-lifecycle-contract PRIVATE voicetyper_asr_lifecycle)
    voicetyper_set_warnings(voicetyper-asr-lifecycle-contract)
    add_test(NAME asr-lifecycle-contract COMMAND voicetyper-asr-lifecycle-contract)
    set_tests_properties(asr-lifecycle-contract PROPERTIES
        PASS_REGULAR_EXPRESSION "asr-lifecycle-contract: OK"
        TIMEOUT 120
    )

    add_executable(voicetyper-native-engine-registry-contract EXCLUDE_FROM_ALL
        tests/contract/native_engine_registry_contract.cpp
    )
    target_link_libraries(voicetyper-native-engine-registry-contract PRIVATE voicetyper_asr_registry)
    voicetyper_set_warnings(voicetyper-native-engine-registry-contract)
    add_test(NAME native-engine-registry-contract COMMAND voicetyper-native-engine-registry-contract)
    set_tests_properties(native-engine-registry-contract PROPERTIES
        PASS_REGULAR_EXPRESSION "native-engine-registry-contract: OK"
    )

    add_executable(voicetyper-model-store-contract EXCLUDE_FROM_ALL
        tests/contract/model_store_contract.cpp
    )
    target_link_libraries(voicetyper-model-store-contract PRIVATE voicetyper_model_store)
    voicetyper_set_warnings(voicetyper-model-store-contract)
    add_test(NAME model-store-contract COMMAND voicetyper-model-store-contract)
    set_tests_properties(model-store-contract PROPERTIES
        PASS_REGULAR_EXPRESSION "model-store-contract: OK"
    )

    add_executable(voicetyper-recording-state-contract EXCLUDE_FROM_ALL
        tests/contract/recording_state_machine_contract.cpp
    )
    target_link_libraries(voicetyper-recording-state-contract PRIVATE voicetyper_state)
    voicetyper_set_warnings(voicetyper-recording-state-contract)
    add_test(NAME recording-state-machine-contract COMMAND voicetyper-recording-state-contract)
    set_tests_properties(recording-state-machine-contract PROPERTIES
        PASS_REGULAR_EXPRESSION "recording-state-machine-contract: OK"
    )

    # Compiles and exercises every portable contract header. It links the
    # domain library for version() and the header-only platform layer; neither
    # target links Qt6, so a GUI-off build stays Qt-free and a header that leaked
    # an OS SDK or Qt include would fail to compile here.
    add_executable(voicetyper-platform-contract-smoke EXCLUDE_FROM_ALL
        tests/contract/platform_contract_smoke.cpp
    )
    target_link_libraries(voicetyper-platform-contract-smoke
        PRIVATE
            voicetyper_domain
            voicetyper_platform
    )
    voicetyper_set_warnings(voicetyper-platform-contract-smoke)
    add_test(NAME platform-contract-smoke COMMAND voicetyper-platform-contract-smoke)
    set_tests_properties(platform-contract-smoke PROPERTIES
        PASS_REGULAR_EXPRESSION "platform-contract-smoke: OK"
    )

    add_executable(voicetyper-executor-contract EXCLUDE_FROM_ALL
        tests/contract/executor_contract.cpp
    )
    target_link_libraries(voicetyper-executor-contract PRIVATE voicetyper_platform)
    voicetyper_set_warnings(voicetyper-executor-contract)
    add_test(NAME executor-contract COMMAND voicetyper-executor-contract)
    set_tests_properties(executor-contract PROPERTIES
        PASS_REGULAR_EXPRESSION "executor-contract: OK"
    )

    if(VOICETYPER_BUILD_GUI)
        # The update core needs QtCore for QJsonDocument, so this test follows the
        # GUI switch like the library it links. It is a plain console binary with
        # the project's own assertion helpers: no QtTest, no network, no display.
        add_executable(voicetyper-update-contract EXCLUDE_FROM_ALL
            tests/contract/update_contract.cpp
        )
        target_link_libraries(voicetyper-update-contract PRIVATE voicetyper_update)
        voicetyper_set_warnings(voicetyper-update-contract)
        add_test(NAME update-contract COMMAND voicetyper-update-contract)
        set_tests_properties(update-contract PROPERTIES
            PASS_REGULAR_EXPRESSION "update-contract: OK"
            TIMEOUT 60
        )
    endif()

    # Re-runs the contract-header include audit as a test, so a header that
    # starts including windows.h/X11/ALSA/Qt fails the suite even on a
    # configuration that was configured before the change.
    add_test(NAME portable-headers
        COMMAND ${CMAKE_COMMAND}
            -DVOICETYPER_SOURCE_DIR=${PROJECT_SOURCE_DIR}
            -P ${PROJECT_SOURCE_DIR}/cmake/PortableHeaders.cmake
    )
    set_tests_properties(portable-headers PROPERTIES
        PASS_REGULAR_EXPRESSION "portable-header check: OK"
    )

    if(Python3_Interpreter_FOUND)
        add_test(NAME fixture-check
            COMMAND ${Python3_EXECUTABLE}
                ${PROJECT_SOURCE_DIR}/tools/verify-migration-fixtures.py
        )
        set_tests_properties(fixture-check PROPERTIES
            PASS_REGULAR_EXPRESSION "fixture-check: OK"
        )
        add_test(NAME differential-compare-smoke
            COMMAND ${Python3_EXECUTABLE}
                ${PROJECT_SOURCE_DIR}/tests/contract/differential_compare_smoke.py
        )
        set_tests_properties(differential-compare-smoke PROPERTIES
            PASS_REGULAR_EXPRESSION "differential-compare-smoke: OK"
        )
    endif()

    add_test(NAME diagnostics-help COMMAND voicetyper-diagnostics --help)
    add_test(NAME diagnostics-version COMMAND voicetyper-diagnostics --version)
    add_test(NAME diagnostics-json COMMAND voicetyper-diagnostics --json)
    add_test(NAME diagnostics-wav-info
        COMMAND voicetyper-diagnostics --wav-info
            ${PROJECT_SOURCE_DIR}/tests/fixtures/migration/wav/wav-sine-44k-mono.wav
    )
    add_test(NAME diagnostics-wav-info-rejects-nonpcm
        COMMAND voicetyper-diagnostics --wav-info
            ${PROJECT_SOURCE_DIR}/tests/fixtures/migration/wav/wav-reject-nonpcm-format28.wav
    )
    set_tests_properties(diagnostics-wav-info PROPERTIES
        PASS_REGULAR_EXPRESSION "\"sampleRate\":44100"
    )
    set_tests_properties(diagnostics-wav-info-rejects-nonpcm PROPERTIES
        WILL_FAIL TRUE
    )
    set_tests_properties(diagnostics-help PROPERTIES
        PASS_REGULAR_EXPRESSION "Usage: voicetyper-diagnostics"
    )
    # Derived from the configured version instead of hard-coded: the literal
    # "0.0.0-dev" went stale the moment the product version became 2.0.0, and the test
    # then failed on Windows for a reason that had nothing to do with the tool.
    string(REPLACE "." "\\." VOICETYPER_VERSION_REGEX "${VOICETYPER_VERSION}")
    set_tests_properties(diagnostics-version PROPERTIES
        PASS_REGULAR_EXPRESSION "${VOICETYPER_VERSION_REGEX}"
    )
    set_tests_properties(diagnostics-json PROPERTIES
        PASS_REGULAR_EXPRESSION "\"name\":\"VoiceTyper"
    )

    # Native dependency contract. Headless, offline, and deliberately without a
    # model: it checks the manifest, the Parakeet ABI constant, the shipped DLL
    # hashes and the loader's explicit unavailable/ABI-6 behaviour. Built in
    # every configuration, including the offline GUI-off one, because the
    # "no silent fallback" rule has to hold where whisper.cpp is not present.
    add_executable(voicetyper-native-dependency-contract EXCLUDE_FROM_ALL
        tests/contract/native_dependency_contract.cpp
    )
    target_link_libraries(voicetyper-native-dependency-contract
        PRIVATE
            voicetyper_domain
            voicetyper_platform
            voicetyper_parakeet_runtime
    )
    target_compile_definitions(voicetyper-native-dependency-contract PRIVATE
        VOICETYPER_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
        VOICETYPER_PARAKEET_ABI_VERSION="${VOICETYPER_PARAKEET_ABI_VERSION}"
        VOICETYPER_PARAKEET_DLL_SHA256="${VOICETYPER_PARAKEET_DLL_SHA256}"
        VOICETYPER_MCWASAPI_DLL_SHA256="${VOICETYPER_MCWASAPI_DLL_SHA256}"
    )
    voicetyper_set_warnings(voicetyper-native-dependency-contract)
    add_test(NAME native-dependency-contract COMMAND voicetyper-native-dependency-contract)
    set_tests_properties(native-dependency-contract PROPERTIES
        PASS_REGULAR_EXPRESSION "native-dependency-contract: OK"
    )

    if(VOICETYPER_BUILD_ASR)
        # Proves the pinned whisper.cpp really is compiled and linked: the test
        # includes the upstream header and calls real C API entry points. It
        # never downloads or loads a model.
        # Scratch probe: can the MME (waveIn) backend hear anything on this machine?
        add_executable(voicetyper-mme-probe EXCLUDE_FROM_ALL
            tools/mme_probe.cpp
        )
        target_link_libraries(voicetyper-mme-probe PRIVATE winmm)
        voicetyper_set_warnings(voicetyper-mme-probe)

        add_executable(voicetyper-whisper-native-contract EXCLUDE_FROM_ALL
            tests/contract/whisper_native_contract.cpp
        )
        target_link_libraries(voicetyper-whisper-native-contract
            PRIVATE
                voicetyper_asr_whisper
                voicetyper_whisper_native
        )
        target_compile_definitions(voicetyper-whisper-native-contract PRIVATE
            VOICETYPER_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
            VOICETYPER_WHISPER_EXPECTED_COMMIT="${VOICETYPER_WHISPER_PINNED_COMMIT}"
            VOICETYPER_WHISPER_EXPECTED_ARCHIVE_SHA256="${VOICETYPER_WHISPER_ARCHIVE_SHA256}"
        )
        voicetyper_set_warnings(voicetyper-whisper-native-contract)
        add_test(NAME whisper-native-contract COMMAND voicetyper-whisper-native-contract)
        set_tests_properties(whisper-native-contract PROPERTIES
            PASS_REGULAR_EXPRESSION "whisper-native-contract: OK"
        )
    endif()
endif()

install(TARGETS voicetyper-diagnostics RUNTIME DESTINATION bin)
if(VOICETYPER_BUILD_GUI)
    install(TARGETS voicetyper-qt-shell RUNTIME DESTINATION bin)
endif()

# ---------------------------------------------------------------------------
# Windows deploy target.
#
# Without this the build tree ships zero DLLs and the app dies in the Windows
# loader with 0xC0000135 before main() runs, which looks like a build failure but
# is not. The rule is deliberately explicit about WHICH runtime is copied: the
# MinGW13 libgcc/libstdc++/libwinpthread from the compiler root, never the newer
# WinLibs copies (the 0xC0000139 shadowing trap), and Qt's DLLs plus the
# qwindows platform plugin from the Qt root.
# ---------------------------------------------------------------------------
if(WIN32 AND VOICETYPER_BUILD_GUI)
    if(DEFINED ENV{VOICETYPER_QT_ROOT})
        file(TO_CMAKE_PATH "$ENV{VOICETYPER_QT_ROOT}" VOICETYPER_QT_ROOT)
    endif()
    if(DEFINED ENV{VOICETYPER_MINGW_ROOT})
        file(TO_CMAKE_PATH "$ENV{VOICETYPER_MINGW_ROOT}" VOICETYPER_MINGW_ROOT)
    endif()

    if(NOT VOICETYPER_QT_ROOT OR NOT VOICETYPER_MINGW_ROOT)
        message(STATUS
            "deploy support: OFF - set VOICETYPER_QT_ROOT and VOICETYPER_MINGW_ROOT "
            "to get a runnable directory next to the executable.")
    else()
        set(VOICETYPER_DEPLOY_DIR "${CMAKE_CURRENT_BINARY_DIR}/deploy" CACHE PATH
            "Directory that receives a runnable application tree")
        find_program(VOICETYPER_WINDEPLOYQT
            NAMES windeployqt6.exe windeployqt.exe
            HINTS "${VOICETYPER_QT_ROOT}/bin")
        find_program(VOICETYPER_WINDEPLOYQT_BIN
            NAMES windeployqt6.exe windeployqt.exe
            HINTS "${VOICETYPER_QT_ROOT}/bin")

        add_custom_target(voicetyper-deploy
            COMMAND ${CMAKE_COMMAND} -E make_directory "${VOICETYPER_DEPLOY_DIR}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:voicetyper-qt-shell>" "${VOICETYPER_DEPLOY_DIR}"
            # The native capture library, exactly as the .NET distribution shipped it.
            # The copy comes from the tracked location (native), which is
            # also the one the manifest check above verifies; dist/ is a build output and
            # is not in the repository.
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${CMAKE_SOURCE_DIR}/native/mc_wasapi.dll" "${VOICETYPER_DEPLOY_DIR}"
            # Qt runtime + the platform plugin, from the Qt root only.
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${VOICETYPER_QT_ROOT}/bin/Qt6Core.dll"
                    "${VOICETYPER_QT_ROOT}/bin/Qt6Gui.dll"
                    "${VOICETYPER_QT_ROOT}/bin/Qt6Widgets.dll"
                    "${VOICETYPER_QT_ROOT}/bin/Qt6Network.dll"
                    "${VOICETYPER_DEPLOY_DIR}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${VOICETYPER_DEPLOY_DIR}/platforms"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${VOICETYPER_QT_ROOT}/plugins/platforms/qwindows.dll"
                    "${VOICETYPER_DEPLOY_DIR}/platforms"
            # Compiler runtime from the MinGW root, never from WinLibs.
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${VOICETYPER_MINGW_ROOT}/bin/libgcc_s_seh-1.dll"
                    "${VOICETYPER_MINGW_ROOT}/bin/libstdc++-6.dll"
                    "${VOICETYPER_MINGW_ROOT}/bin/libwinpthread-1.dll"
                    "${VOICETYPER_DEPLOY_DIR}"
            # Parakeet is runtime-loaded beside the executable, not imported, so
            # nothing in the link line would copy it for us.
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${PROJECT_SOURCE_DIR}/native/parakeet.dll"
                    "${VOICETYPER_DEPLOY_DIR}"
            COMMENT "Assembling a runnable tree in ${VOICETYPER_DEPLOY_DIR}"
            VERBATIM)
        message(STATUS
            "deploy support: ON - build target voicetyper-deploy writes a runnable tree to "
            "${VOICETYPER_DEPLOY_DIR}")
    endif()
endif()

# ---------------------------------------------------------------------------
# Windows capture backend (plan p_312b2ec83985 Phase D, task t_bec8c2127352).
#
# This block is WIN32-only and appended, so a Linux/macOS configure never sees
# it and the file's existing target order is untouched. src/platform/windows is
# the backend layer behind src/platform/api/{audio_capture,microphone}.hpp, so
# these sources are the first place the WASAPI/COM surface enters the build.
#
# Deliberately *not* bound here: mc_wasapi.dll (no ABI contract yet), the MME
# fallback, and any Qt type. The adapter is driven through the abstract
# domain::RecordingPort, which is what keeps the conversion testable without a
# microphone and keeps the state machine free of COM.
# ---------------------------------------------------------------------------
if(WIN32)
    add_library(voicetyper_audio_capture STATIC
        src/platform/windows/wasapi_capture.cpp
        src/platform/windows/windows_microphone.cpp
        src/platform/mc_wasapi.cpp
        src/platform/windows/windows_audio_capture.cpp
    )
    target_link_libraries(voicetyper_audio_capture
        PUBLIC
            voicetyper_audio
            voicetyper_state
        PRIVATE
            # COM + the audio engine. ole32 for CoCreateInstance/CoTaskMemFree,
            # propsys for the endpoint property store (friendly names), uuid for
            # the interface identifiers.
            ole32
            propsys
            uuid
    )
    voicetyper_set_warnings(voicetyper_audio_capture)

    if(VOICETYPER_BUILD_TESTS)
        # Real-device evidence. On a machine without a usable capture endpoint the
        # test prints its reason and exits 77, which CTest reports as skipped -
        # so the suite stays green on a headless/CI box and the test can never
        # hang: every wait inside it is bounded.
        add_executable(voicetyper-windows-audio-capture-contract EXCLUDE_FROM_ALL
            tests/contract/windows_audio_capture_contract.cpp
        )
        target_link_libraries(voicetyper-windows-audio-capture-contract
            PRIVATE
                voicetyper_audio_capture
        )
        voicetyper_set_warnings(voicetyper-windows-audio-capture-contract)
        add_test(NAME windows-audio-capture-contract
            COMMAND voicetyper-windows-audio-capture-contract
        )
        set_tests_properties(windows-audio-capture-contract PROPERTIES
            LABELS "windows;audio;hardware"
            PASS_REGULAR_EXPRESSION "windows-audio-capture-contract: (OK|SKIP)"
            SKIP_RETURN_CODE 77
            TIMEOUT 120
        )
    endif()
endif()

# ---------------------------------------------------------------------------
# Windows platform backends: clock, clipboard, paste, executor, filesystem,
# logger (plan p_312b2ec83985 Phase D).
#
# Appended and WIN32-only, so a Linux/macOS configure never sees these sources
# and the file's existing target order is untouched. src/platform/windows is the
# backend layer behind src/platform/api/{clock,clipboard,paste,executor,
# file_system,logger}.hpp: every one of the six classes implements an interface
# that already exists and is already contract-tested portably, so this block
# adds a platform binding and no new contract surface.
#
# The headers stay standard-C++20 with no <windows.h>, exactly like
# src/platform/windows/parakeet_runtime.hpp, so a portable test can include them
# and assert the "unavailable off Windows" path; only the .cpp files include the
# Win32 headers, and only here do the OS libraries appear:
#   * user32   - the hidden clipboard owner window, its message loop,
#                OpenInputDesktop/CloseDesktop for the paste capability probe;
#   * advapi32 - OpenProcessToken/GetTokenInformation for the read-only paste
#                integrity probe that turns the UAC case into permission_denied
#                instead of an opaque SendInput failure.
# kernel32 (QPC, FILETIME, MoveFileExW, GetDiskFreeSpaceExW) is linked by default.
# ---------------------------------------------------------------------------
if(WIN32)
    add_library(voicetyper_platform_win32 STATIC
        src/platform/windows/win32_clock.cpp
        src/platform/windows/win32_clipboard.cpp
        src/platform/windows/win32_executor.cpp
        src/platform/windows/win32_file_system.cpp
        src/platform/windows/win32_logger.cpp
        src/platform/windows/win32_paste.cpp
    )
    target_link_libraries(voicetyper_platform_win32
        PUBLIC
            voicetyper_domain
            voicetyper_platform
            # domain::AppPaths, used by Win32Logger::for_paths.
            voicetyper_core_support
        PRIVATE
            advapi32
            user32
    )
    voicetyper_set_warnings(voicetyper_platform_win32)

    if(VOICETYPER_BUILD_TESTS)
        # Real-environment evidence with no desktop interaction: a pre-cancelled
        # clock token, the atomic_write temp-then-move shape, create_new refusal,
        # the streaming writer, the log line format with an injected clock, the
        # executor running a task on its own thread (and not deadlocking when
        # invoked from that same thread), and the clipboard/paste backends
        # reporting unavailable instead of crashing when no desktop session can
        # provide a device. Every wait is bounded and the scratch directory is
        # removed on every exit path, so the test can neither hang nor litter.
        # A real SendInput Ctrl+V is deliberately *not* injected here: it would
        # type into whatever window is focused while the suite runs. That belongs
        # to the physical Windows platform gate.
        add_executable(voicetyper-win32-platform-contract EXCLUDE_FROM_ALL
            tests/contract/win32_platform_contract.cpp
        )
        target_link_libraries(voicetyper-win32-platform-contract
            PRIVATE
                voicetyper_platform_win32
        )
        voicetyper_set_warnings(voicetyper-win32-platform-contract)
        add_test(NAME win32-platform-contract
            COMMAND voicetyper-win32-platform-contract
        )
        set_tests_properties(win32-platform-contract PROPERTIES
            LABELS "windows;platform;hardware"
            PASS_REGULAR_EXPRESSION "win32-platform-contract: OK"
            TIMEOUT 120
        )
    endif()
endif()

# ---------------------------------------------------------------------------
# Windows input backends: global hotkeys, the WH_KEYBOARD_LL capture hook and
# the XInput gamepad poll (plan p_312b2ec83985 Phase D, task t_c0fb9490a5e4).
#
# Appended and WIN32-only, so a Linux/macOS configure never sees these sources
# and the existing target order is untouched. A separate target from
# voicetyper_platform_win32 on purpose: that target is one agent's file, and two
# agents appending to one source list is a conflict waiting to happen. This block
# only reads it, and both libraries can later be folded together deliberately.
#
# src/platform/windows is the backend layer behind
# src/platform/api/{hotkeys,gamepad}.hpp: Win32HotkeyService, HotkeyCaptureHook
# and Win32GamepadService implement interfaces that already exist and are already
# contract-tested portably, so this adds a platform binding and no new contract
# surface.
#
# The two headers stay standard-C++20 with no <windows.h>, exactly like
# src/platform/windows/parakeet_runtime.hpp, so a portable test can include them;
# only the .cpp files include the Win32 headers, and only here do the OS
# libraries appear:
#   * user32   - RegisterHotKey/UnregisterHotKey and the WM_HOTKEY pump,
#                SetWindowsHookExW(WH_KEYBOARD_LL)/CallNextHookEx, PeekMessage/
#                MsgWaitForMultipleObjects/PostThreadMessage, SetTimer,
#                GetAsyncKeyState for the push-to-talk release poll, and
#                LoadLibraryW for the XInput runtime;
#   * advapi32 - the read-only hotkey policy probe (RegGetValueW) and the token
#                integrity label behind the UAC-aware conflict diagnostic.
# kernel32 (events, threads, tokens) is linked by default. XInput is *not* a
# link-time dependency: xinput1_4/1_3/9_1_0 are resolved at runtime so a machine
# without any XInput runtime reports unavailable with a reason instead of failing
# to start.
# ---------------------------------------------------------------------------
if(WIN32)
    add_library(voicetyper_platform_win32_input STATIC
        src/platform/windows/win32_hotkeys.cpp
        src/platform/windows/win32_gamepad.cpp
    )
    target_link_libraries(voicetyper_platform_win32_input
        PUBLIC
            voicetyper_domain
            voicetyper_platform
        PRIVATE
            advapi32
            user32
    )
    voicetyper_set_warnings(voicetyper_platform_win32_input)

    if(VOICETYPER_BUILD_TESTS)
        # Real-environment evidence with no device and no key press: the VK table
        # round-trip, the policy probe and its "blocked by policy" rule, a
        # RegisterHotKey round-trip on a synthetic combination with a forced
        # conflict proven across threads, an idempotent release that leaves
        # nothing registered, the WH_KEYBOARD_LL hook installing and joining
        # inside a deadline, and the XInput poll driven by a fake pad that
        # debounces edges, bounds re-enumeration with no controller, and answers
        # a capture with device_disconnected. Every wait is bounded, so the test
        # cannot hang, and every hotkey it registers is released on every path.
        # A scenario the environment cannot provide (no interactive window
        # station, a policy that refuses the synthetic combination) prints a
        # skip reason and still passes.
        add_executable(voicetyper-win32-input-contract EXCLUDE_FROM_ALL
            tests/contract/win32_input_contract.cpp
        )
        target_link_libraries(voicetyper-win32-input-contract
            PRIVATE
                voicetyper_platform_win32_input
        )
        voicetyper_set_warnings(voicetyper-win32-input-contract)
        add_test(NAME win32-input-contract
            COMMAND voicetyper-win32-input-contract
        )
        set_tests_properties(win32-input-contract PROPERTIES
            LABELS "windows;input;hardware"
            PASS_REGULAR_EXPRESSION "win32-input-contract: OK"
            TIMEOUT 120
        )
    endif()
endif()

# ---------------------------------------------------------------------------
# Windows autostart backend: the HKCU Run key behind "Запускать вместе с
# Windows" (plan p_312b2ec83985 Phase D, task t_c0e3b56d32d2, parity row
# LIFE-01).
#
# Appended and WIN32-only, like the two blocks above, so a Linux/macOS
# configure never sees these sources and the existing target order is
# untouched. A separate target from voicetyper_platform_win32 and
# voicetyper_platform_win32_input on purpose: those source lists belong to
# other slices, and this block only links them.
#
# src/platform/windows/win32_startup.{hpp,cpp} is the backend for the autostart
# contract of compatibility-contracts.md section 2: HKCU
# Software\Microsoft\Windows\CurrentVersion\Run, value "VoiceTyper", the quoted
# current executable, plus " --start-minimized" when startMinimized is on. The
# header is standard-C++20 with no <windows.h>, so the quoting rules, the
# command-line builder and the switch parser stay portable; only the .cpp
# includes the Win32 headers, and only here do the OS libraries appear:
#   * advapi32 - RegOpenKeyExW/RegQueryValueExW/RegSetValueExW/RegDeleteValueW,
#                plus RegCreateKeyExW for a profile whose Run key is missing.
# kernel32 (GetModuleFileNameW, WideCharToMultiByte) is linked by default.
# ---------------------------------------------------------------------------
if(WIN32)
    add_library(voicetyper_platform_win32_startup STATIC
        src/platform/windows/win32_startup.cpp
    )
    target_link_libraries(voicetyper_platform_win32_startup
        PUBLIC
            voicetyper_domain
            voicetyper_platform
        PRIVATE
            advapi32
    )
    voicetyper_set_warnings(voicetyper_platform_win32_startup)

    # The Windows composition reconciles the Run key on startup and on every
    # applied settings change. Added as its own link line rather than an edit
    # to the existing voicetyper-qt-shell block, so nothing above is reordered
    # or rewritten.
    if(VOICETYPER_BUILD_GUI AND TARGET voicetyper-qt-shell)
        target_link_libraries(voicetyper-qt-shell
            PRIVATE
                voicetyper_platform_win32_startup
        )
    endif()

    if(VOICETYPER_BUILD_TESTS)
        # Real-registry evidence with no restart and no user interaction: the
        # value round-trips through the actual HKCU Run key as REG_SZ, enabling
        # twice is byte-identical, disabling leaves no value behind (verified
        # with a raw RegQueryValueExW, not only through the backend), a missing
        # value reads as disabled, and the command line is the quoted
        # executable. Every write and read assertion uses a distinct
        # per-process value name in the production key, and the test asserts
        # the production "VoiceTyper" value is byte-identical before and after,
        # so a real installed app's autostart entry can neither be disturbed
        # nor be left pointing at a test build. The test contains no wait at
        # all; the TIMEOUT is the outer bound.
        add_executable(voicetyper-win32-startup-contract EXCLUDE_FROM_ALL
            tests/contract/win32_startup_contract.cpp
        )
        target_link_libraries(voicetyper-win32-startup-contract
            PRIVATE
                voicetyper_platform_win32_startup
                # The test reads the registry itself to check the backend
                # against the machine rather than against itself.
                advapi32
        )
        voicetyper_set_warnings(voicetyper-win32-startup-contract)
        add_test(NAME win32-startup-contract
            COMMAND voicetyper-win32-startup-contract
        )
        set_tests_properties(win32-startup-contract PROPERTIES
            LABELS "windows;lifecycle;registry"
            PASS_REGULAR_EXPRESSION "win32-startup-contract: OK"
            TIMEOUT 60
        )
    endif()
endif()

# ---------------------------------------------------------------------------
# Windows update hand-off (plan p_a95b558c6861, Phase 3).
#
# src/platform/windows/win32_update_launcher.{hpp,cpp} is the backend behind
# core/support/update_launcher.hpp: it writes run-update.cmd and starts it with
# CREATE_NO_WINDOW, exactly as UpdateLauncher.cs did. Only the Win32 call is
# WIN32-only; the script bytes are portable and covered by
# voicetyper-update-contract, so this block has no test of its own - the normal
# Windows build compiles it, and Phase 4 adds the call site.
# ---------------------------------------------------------------------------
if(WIN32 AND VOICETYPER_BUILD_GUI)
    add_library(voicetyper_platform_win32_update_launcher STATIC
        src/platform/windows/win32_update_launcher.cpp
    )
    target_link_libraries(voicetyper_platform_win32_update_launcher
        PUBLIC
            voicetyper_domain
            voicetyper_platform
            voicetyper_update
    )
    voicetyper_set_warnings(voicetyper_platform_win32_update_launcher)
endif()

# The default build produces exactly one executable: the application. Every test and
# tool target is EXCLUDE_FROM_ALL, because a plain build used to leave two dozen
# contract tests next to the app in the build directory, which reads as if the project
# shipped a dozen programs (Alexander, 2026-10-06) and made the build slow.
# Build them on demand with:
#   cmake --build <build-dir> --target voicetyper-tests
add_custom_target(voicetyper-tests
    DEPENDS
        voicetyper-asr-native-smoke
        voicetyper-asr-whisper-probe
        voicetyper-diagnostics
        voicetyper-domain-smoke
        voicetyper-settings-json-contract
        voicetyper-core-support-contract
        voicetyper-audio-dsp-contract
        voicetyper-vad-contract
        voicetyper-mc-wasapi-contract
        voicetyper-microphone-level-contract
        voicetyper-capture-guard-contract
        voicetyper-update-service-contract
        voicetyper-asr-parameters-contract
        voicetyper-ui-settings-test
        voicetyper-ui-snapshot
        voicetyper-ui-status-overlay-test
        voicetyper-settings-presenter-contract
        voicetyper-text-output-contract
        voicetyper-asr-lifecycle-contract
        voicetyper-native-engine-registry-contract
        voicetyper-model-store-contract
        voicetyper-recording-state-contract
        voicetyper-platform-contract-smoke
        voicetyper-executor-contract
        voicetyper-update-contract
        voicetyper-native-dependency-contract
        voicetyper-whisper-native-contract
        voicetyper-windows-audio-capture-contract
        voicetyper-win32-platform-contract
        voicetyper-win32-input-contract
        voicetyper-win32-startup-contract
)
