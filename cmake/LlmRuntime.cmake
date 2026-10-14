# ---------------------------------------------------------------------------
# Local text-cleanup runtime (llama.cpp behind voicetyper_llm).
#
# The product's optional cleanup model runs on llama.cpp. It is vendored by
# commit (native/llm/LlamaPins.cmake holds the URL, the commit, the archive
# SHA-256 and the content pins of the files that carry the API and the build),
# downloaded and hash-verified, and built into ONE shared library with our thin
# facade inside it:
#
#   voicetyper_llm.so / voicetyper_llm.dll
#       = native/llm/voicetyper_llm.{h,cpp} + llama.cpp + ggml (both static)
#
# Why a separate CMake tree (ExternalProject), like parakeet.cpp and
# transcribe.cpp:
#   * llama.cpp declares the targets `ggml`, `ggml-base` and `ggml-cpu`, and so
#     does the whisper.cpp the application already links statically - one CMake
#     project cannot hold both, and llama.cpp's own top-level CMakeLists skips
#     its ggml when a target of that name exists, which would silently bind
#     llama.cpp 0.6.0 (ggml 0.26.0) to whisper.cpp's ggml 0.25.1;
#   * a genuinely separate tree cannot leak llama.cpp's sub-build flags,
#     install rules or warnings into the application build;
#   * the application therefore reaches llama.cpp only through
#     native/llm/voicetyper_llm.h, exactly like the other native engines.
#
# What this module defines:
#   voicetyper-llm-native            build the shared library (network once)
#   voicetyper_llm_runtime           portable static loader (dlopen/LoadLibrary)
#   voicetyper-llm-runtime-contract  ctest: load a tiny GGUF and infer once
#
# Nothing here is part of the default build: the ExternalProject is
# EXCLUDE_FROM_ALL, so a plain `cmake --build <dir>` behaves exactly as before
# until someone asks for the runtime or for the contract test.
# ---------------------------------------------------------------------------

include(ExternalProject)

function(voicetyper_add_llm_runtime)
    include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../native/llm/LlamaPins.cmake")
    voicetyper_llm_verify_llama_pin_shape()

    set(llm_library_dir "${CMAKE_BINARY_DIR}/llm-libs")
    set(llm_library_name "voicetyper_llm${CMAKE_SHARED_LIBRARY_SUFFIX}")
    set(llm_library_path "${llm_library_dir}/${llm_library_name}")

    # Offline escape hatch, the same shape as VOICETYPER_WHISPER_PREFETCHED_DIR:
    # a Windows CMake without a usable CA store (libcurl code 60) or an
    # air-gapped runner can be handed an already extracted tree. It is not a
    # weaker contract - native/llm/CMakeLists.txt re-hashes the content pins
    # inside it, so only the pinned commit is accepted.
    # The environment variable is authoritative when it is set, the same way the
    # MinGW toolchain file treats VOICETYPER_MINGW_ROOT/VOICETYPER_QT_ROOT. That
    # matters because the Windows build is driven by a fixed helper command
    # (`cmd /c vt-win-build.cmd <dir> <target>`) that cannot carry extra -D
    # arguments, and because CMake's libcurl on that machine cannot verify
    # github.com (observed: the download step fails, exactly as it does for
    # whisper.cpp, which is why that pin uses a pre-fetched tree too).
    if(DEFINED ENV{VOICETYPER_LLM_PREFETCHED_DIR} AND NOT "$ENV{VOICETYPER_LLM_PREFETCHED_DIR}" STREQUAL "")
        set(VOICETYPER_LLM_PREFETCHED_DIR "$ENV{VOICETYPER_LLM_PREFETCHED_DIR}" CACHE PATH
            "Pre-fetched llama.cpp tree for builds without network access; its content is still verified against the pins in native/llm/LlamaPins.cmake"
            FORCE)
    else()
        set(VOICETYPER_LLM_PREFETCHED_DIR "" CACHE PATH
            "Pre-fetched llama.cpp tree for builds without network access; its content is still verified against the pins in native/llm/LlamaPins.cmake")
    endif()

    if(VOICETYPER_LLM_PREFETCHED_DIR)
        # `set VAR=value && next` in cmd.exe - which is how the Windows build hands this path over
        # - leaves a trailing space in the value, and a path with one points at a directory that
        # does not exist. Observed: CMake reported "is not a llama.cpp tree" for a perfectly good
        # tree. The value is therefore trimmed, and a path that still does not exist is reported
        # as such instead of being blamed on the tree.
        string(STRIP "${VOICETYPER_LLM_PREFETCHED_DIR}" VOICETYPER_LLM_PREFETCHED_DIR)
        set(VOICETYPER_LLM_PREFETCHED_DIR "${VOICETYPER_LLM_PREFETCHED_DIR}" CACHE PATH
            "Pre-fetched llama.cpp tree for builds without network access; its content is still verified against the pins in native/llm/LlamaPins.cmake"
            FORCE)
        if(NOT IS_DIRECTORY "${VOICETYPER_LLM_PREFETCHED_DIR}")
            message(FATAL_ERROR
                "VOICETYPER_LLM_PREFETCHED_DIR does not exist: "
                "\"${VOICETYPER_LLM_PREFETCHED_DIR}\"")
        endif()
        if(NOT EXISTS "${VOICETYPER_LLM_PREFETCHED_DIR}/CMakeLists.txt")
            message(FATAL_ERROR
                "VOICETYPER_LLM_PREFETCHED_DIR is set but is not a llama.cpp tree: "
                "${VOICETYPER_LLM_PREFETCHED_DIR}")
        endif()
        message(STATUS
            "llama.cpp: using the pre-fetched tree ${VOICETYPER_LLM_PREFETCHED_DIR} (no download); "
            "the manifest content pins are still verified")
        set(llama_source_dir "${VOICETYPER_LLM_PREFETCHED_DIR}")
        set(llama_download_target "")
    else()
        # A download-only ExternalProject: it fetches the commit archive, checks
        # it against URL_HASH and extracts it, and does nothing else. The
        # wrapper project then verifies the extracted tree against the content
        # pins, so a re-cut archive cannot pass for the pinned commit.
        ExternalProject_Add(voicetyper_llama_cpp_source
            URL "${VOICETYPER_LLAMA_ARCHIVE_URL}"
            URL_HASH "SHA256=${VOICETYPER_LLAMA_ARCHIVE_SHA256}"
            DOWNLOAD_EXTRACT_TIMESTAMP OFF
            PREFIX "${CMAKE_BINARY_DIR}/llama-cpp-source"
            UPDATE_COMMAND ""
            CONFIGURE_COMMAND ""
            BUILD_COMMAND ""
            INSTALL_COMMAND ""
            TEST_COMMAND ""
            LOG_DOWNLOAD ON
            EXCLUDE_FROM_ALL
        )
        set(llama_source_dir "${CMAKE_BINARY_DIR}/llama-cpp-source/src/voicetyper_llama_cpp_source")
        set(llama_download_target voicetyper_llama_cpp_source)
    endif()

    set(llm_cmake_args
        "-DCMAKE_BUILD_TYPE=Release"
        "-DCMAKE_INSTALL_PREFIX=${llm_library_dir}"
        "-DVOICETYPER_LLAMA_SOURCE_DIR=${llama_source_dir}"
        "-DCMAKE_POSITION_INDEPENDENT_CODE=ON"
    )
    if(WIN32)
        # The wrapper tree is configured natively on Windows (the main build is
        # a MinGW-hosted build, not a cross build from Linux), so it gets the
        # compilers and the Ninja driver explicitly instead of the toolchain
        # file - the toolchain sets a find-root that would hide the headers
        # llama.cpp looks for.
        list(APPEND llm_cmake_args
            "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}"
            "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
            "-DCMAKE_RC_COMPILER=${CMAKE_RC_COMPILER}"
            "-DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
        )
    endif()

    ExternalProject_Add(voicetyper-llm-native
        SOURCE_DIR "${PROJECT_SOURCE_DIR}/native/llm"
        PREFIX "${CMAKE_BINARY_DIR}/llm-runtime"
        CMAKE_ARGS ${llm_cmake_args}
        UPDATE_COMMAND ""
        BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --config Release --parallel
        INSTALL_COMMAND "${CMAKE_COMMAND}" --install <BINARY_DIR> --config Release
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${llm_library_path}" "${CMAKE_BINARY_DIR}/${llm_library_name}"
        BUILD_BYPRODUCTS "${llm_library_path}"
        LOG_CONFIGURE ON
        LOG_BUILD ON
        LOG_INSTALL ON
        DEPENDS ${llama_download_target}
        EXCLUDE_FROM_ALL
    )

    # Portable loader: dlsym/LoadLibraryW, a closed symbol list, ABI and pin
    # assertions, domain::Result mapping. Portable on purpose, so the "library
    # absent / foreign ABI" contract can be tested anywhere.
    add_library(voicetyper_llm_runtime STATIC
        native/llm/llm_runtime.cpp
    )
    target_include_directories(voicetyper_llm_runtime
        PUBLIC "${PROJECT_SOURCE_DIR}/native/llm"
    )
    target_link_libraries(voicetyper_llm_runtime
        PUBLIC
            voicetyper_domain
            voicetyper_platform
    )
    target_compile_definitions(voicetyper_llm_runtime PRIVATE
        VOICETYPER_LLAMA_PINNED_COMMIT="${VOICETYPER_LLAMA_PINNED_COMMIT}"
    )
    if(NOT WIN32)
        # dlopen/dlclose live in libdl on the older glibc and in libc on newer
        # ones; CMAKE_DL_LIBS is empty where it is not needed.
        target_link_libraries(voicetyper_llm_runtime PUBLIC ${CMAKE_DL_LIBS})
    endif()
    voicetyper_set_warnings(voicetyper_llm_runtime)

    # The domain port implementation (src/domain/cleanup_runtime.hpp): prompt,
    # sampling parameters and answer hygiene on top of the loader. Portable C++
    # with no llama.h, so the domain layer can link it on both platforms and a
    # test can drive it without knowing about the native library.
    add_library(voicetyper_llm_cleanup STATIC
        native/llm/llama_cleanup_runtime.cpp
    )
    target_link_libraries(voicetyper_llm_cleanup
        PUBLIC
            voicetyper_domain
            voicetyper_llm_runtime
    )
    voicetyper_set_warnings(voicetyper_llm_cleanup)

    # Real-runtime evidence. Opt-in by data (VOICETYPER_LLM_MODEL) and by name:
    # it is not in the voicetyper-tests aggregate, because building it fetches
    # and compiles llama.cpp - a build-wide test target must not gain a network
    # dependency.
    add_executable(voicetyper-llm-runtime-contract EXCLUDE_FROM_ALL
        native/llm/tests/llm_runtime_contract.cpp
    )
    target_link_libraries(voicetyper-llm-runtime-contract
        PRIVATE voicetyper_llm_runtime voicetyper_llm_cleanup
    )
    voicetyper_set_warnings(voicetyper-llm-runtime-contract)
    add_dependencies(voicetyper-llm-runtime-contract voicetyper-llm-native)
    add_test(NAME llm-runtime-contract COMMAND voicetyper-llm-runtime-contract)
    set_tests_properties(llm-runtime-contract PROPERTIES
        LABELS "llm;native;local-model"
        PASS_REGULAR_EXPRESSION "llm-runtime-contract: (OK|SKIP)"
        TIMEOUT 900
    )

    # Deployment: the loader looks for the library beside the executable, and the
    # runtime component of an installed tree carries it too. install(FILES) is
    # evaluated at install time, so a build that never produced the library
    # simply installs without it.
    install(FILES "${llm_library_path}"
        DESTINATION bin
        COMPONENT runtime
        OPTIONAL
    )

    message(STATUS
        "llama.cpp: ${VOICETYPER_LLAMA_PINNED_COMMIT} (${VOICETYPER_LLAMA_PINNED_TAG}, upstream "
        "${VOICETYPER_LLAMA_UPSTREAM_VERSION}, ggml ${VOICETYPER_LLAMA_GGML_VERSION}); "
        "build it with --target voicetyper-llm-native (archive sha256 "
        "${VOICETYPER_LLAMA_ARCHIVE_SHA256})")
endfunction()
