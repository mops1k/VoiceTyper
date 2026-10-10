# Native ASR dependency integration.
#
# Two different mechanisms, deliberately kept apart:
#
#   * whisper.cpp is *compiled from source* into the binary. It is fetched with
#     FetchContent from a content-addressed archive of one pinned commit and
#     re-exposed as the target `voicetyper_whisper_native`. The pin lives in
#     docs/migration/cpp/native-dependencies.json, which this module reads; the
#     build therefore cannot drift from the documented pin, and a mismatch
#     between the manifest and the portable registry header fails configure
#     instead of quietly changing engine behaviour.
#
#   * parakeet.cpp is a *prebuilt DLL* that ships with the application. It is
#     never linked at build time; src/platform/windows/parakeet_runtime.cpp
#     resolves it with LoadLibraryW/GetProcAddress and asserts ABI version 6.
#     Nothing in this file downloads, rebuilds or replaces that DLL, and no
#     unreviewed binary is ever fetched.
#
# No engine is ever silently substituted: this module only makes the pinned
# dependency available. Choosing an engine, refusing an unavailable one and
# reporting a precise state is the job of the engine registry
# (src/platform/api/engine_registry.hpp) and the Phase C lifecycle task.
#
# Network access is required only when VOICETYPER_BUILD_ASR=ON. A GUI-off
# portable contract build keeps it OFF and stays completely offline.

include_guard(GLOBAL)

set(VOICETYPER_NATIVE_DEPS_MANIFEST "${PROJECT_SOURCE_DIR}/docs/migration/cpp/native-dependencies.json"
    CACHE FILEPATH "Machine-readable native dependency manifest (repository, commit, license, ABI, hashes)")

# --- manifest access ---------------------------------------------------------
#
# The manifest is the single source of truth. Every value used below is read
# from it, and a missing or empty field is a hard configure error rather than a
# blank that would silently turn into an unverified build.

# Reads a value out of the manifest. Usage:
#   voicetyper_native_manifest_get(<dependency id> <out_var> [<member> ...])
# where the trailing members form the path inside the dependency object, so
#   voicetyper_native_manifest_get(whisper.cpp out integration archive sha256)
# reads dependencies[whisper.cpp].integration.archive.sha256.
function(voicetyper_native_manifest_get id out_var)
    if(NOT EXISTS "${VOICETYPER_NATIVE_DEPS_MANIFEST}")
        message(FATAL_ERROR
            "native dependency manifest not found: ${VOICETYPER_NATIVE_DEPS_MANIFEST}")
    endif()
    file(READ "${VOICETYPER_NATIVE_DEPS_MANIFEST}" manifest_text)
    string(JSON dep_count ERROR_VARIABLE count_error LENGTH "${manifest_text}" dependencies)
    if(count_error)
        message(FATAL_ERROR "native-dependencies.json: cannot read 'dependencies': ${count_error}")
    endif()
    if(dep_count EQUAL 0)
        message(FATAL_ERROR "native-dependencies.json: 'dependencies' is empty")
    endif()

    math(EXPR last_index "${dep_count} - 1")
    foreach(index RANGE ${last_index})
        string(JSON dep_id ERROR_VARIABLE id_error GET "${manifest_text}" dependencies ${index} id)
        if(id_error)
            message(FATAL_ERROR "native-dependencies.json dependencies[${index}]: ${id_error}")
        endif()
        if(dep_id STREQUAL id)
            if(NOT ARGN)
                set(${out_var} "${dep_id}" PARENT_SCOPE)
                return()
            endif()
            string(JSON value ERROR_VARIABLE field_error GET "${manifest_text}" dependencies ${index} ${ARGN})
            if(field_error)
                string(REPLACE ";" "." readable_path "${ARGN}")
                message(FATAL_ERROR
                    "native-dependencies.json dependency '${id}' has no '${readable_path}': ${field_error}")
            endif()
            if(value STREQUAL "")
                string(REPLACE ";" "." readable_path "${ARGN}")
                message(FATAL_ERROR
                    "native-dependencies.json dependency '${id}' has an empty '${readable_path}'")
            endif()
            set(${out_var} "${value}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    message(FATAL_ERROR "native-dependencies.json has no dependency with id '${id}'")
endfunction()

# Reads `dependencies[?].artifacts[j].<field>` for the artifact whose `path`
# matches <artifact_path> inside dependency <id>.
function(voicetyper_native_manifest_artifact id artifact_path field out_var)
    file(READ "${VOICETYPER_NATIVE_DEPS_MANIFEST}" manifest_text)
    string(JSON dep_count ERROR_VARIABLE count_error LENGTH "${manifest_text}" dependencies)
    if(count_error)
        message(FATAL_ERROR "native-dependencies.json: cannot read 'dependencies': ${count_error}")
    endif()
    math(EXPR last_index "${dep_count} - 1")
    foreach(index RANGE ${last_index})
        string(JSON dep_id GET "${manifest_text}" dependencies ${index} id)
        if(dep_id STREQUAL id)
            string(JSON artifact_count ERROR_VARIABLE artifact_error LENGTH
                "${manifest_text}" dependencies ${index} artifacts)
            if(artifact_error)
                message(FATAL_ERROR "native-dependencies.json dependency '${id}': ${artifact_error}")
            endif()
            math(EXPR last_artifact "${artifact_count} - 1")
            foreach(artifact_index RANGE ${last_artifact})
                string(JSON candidate GET
                    "${manifest_text}" dependencies ${index} artifacts ${artifact_index} path)
                if(candidate STREQUAL artifact_path)
                    string(JSON value GET
                        "${manifest_text}" dependencies ${index} artifacts ${artifact_index} ${field})
                    if(value STREQUAL "")
                        message(FATAL_ERROR
                            "native-dependencies.json artifact '${artifact_path}' has an empty '${field}'")
                    endif()
                    set(${out_var} "${value}" PARENT_SCOPE)
                    return()
                endif()
            endforeach()
            message(FATAL_ERROR
                "native-dependencies.json dependency '${id}' has no artifact '${artifact_path}'")
        endif()
    endforeach()
    message(FATAL_ERROR "native-dependencies.json has no dependency with id '${id}'")
endfunction()

# --- shipped native DLL verification ----------------------------------------
#
# Runs at configure time on any host that has the repository checkout, so a
# substituted or truncated DLL becomes a configure error instead of a runtime
# surprise on the user's machine. A missing file is reported, not fatal: a
# source-only checkout still configures, and the `native-dependency-contract`
# CTest then reports that fact instead of claiming verification.

function(voicetyper_verify_shipped_dll id artifact_path)
    voicetyper_native_manifest_artifact(${id} "${artifact_path}" sha256 expected_sha256)
    voicetyper_native_manifest_artifact(${id} "${artifact_path}" bytes expected_bytes)
    set(absolute "${PROJECT_SOURCE_DIR}/${artifact_path}")
    if(NOT EXISTS "${absolute}")
        message(STATUS
            "native dependency ${id}: ${artifact_path} absent from this checkout "
            "(manifest expects ${expected_bytes} bytes) - configure-time verification skipped")
        return()
    endif()
    file(SIZE "${absolute}" actual_bytes)
    if(NOT actual_bytes EQUAL expected_bytes)
        message(FATAL_ERROR
            "native dependency ${id}: ${artifact_path} is ${actual_bytes} bytes, "
            "the manifest expects ${expected_bytes} bytes.\n"
            "Refusing to build against an unreviewed or truncated native library.")
    endif()
    file(SHA256 "${absolute}" actual_sha256)
    if(NOT actual_sha256 STREQUAL expected_sha256)
        message(FATAL_ERROR
            "native dependency ${id}: ${artifact_path} SHA-256 is ${actual_sha256}, "
            "the manifest expects ${expected_sha256}.\n"
            "Refusing to build against an unreviewed or replaced native library.")
    endif()
    set(verified TRUE PARENT_SCOPE)
    set(sha256 "${actual_sha256}" PARENT_SCOPE)
    set(bytes "${actual_bytes}" PARENT_SCOPE)
    message(STATUS
        "native dependency ${id}: ${artifact_path} verified (${actual_bytes} bytes, sha256 ${actual_sha256})")
endfunction()

# --- pin cross-checks --------------------------------------------------------

# The Parakeet pin and ABI version exist in two places on purpose: as data in
# the manifest (machine-readable, hashed, license-carrying) and as constants in
# the portable registry header (compile-time, usable from platform-free code).
# They must agree. This check runs on every configure - GUI-off, offline, with or
# without ASR - so a port that edits one without the other fails immediately
# instead of producing a build whose manifest and constants disagree.
function(voicetyper_verify_native_pins)
    voicetyper_native_manifest_get(parakeet.cpp manifest_abi abiVersion)
    voicetyper_native_manifest_get(parakeet.cpp manifest_commit pinnedCommit)

    set(registry "${PROJECT_SOURCE_DIR}/src/platform/api/engine_registry.hpp")
    file(READ "${registry}" registry_text)
    if(NOT registry_text MATCHES "kParakeetAbiVersion = ${manifest_abi}")
        message(FATAL_ERROR
            "engine_registry.hpp does not freeze kParakeetAbiVersion = ${manifest_abi} "
            "as required by native-dependencies.json")
    endif()
    if(NOT registry_text MATCHES "kParakeetPinnedCommit = \"${manifest_commit}\"")
        message(FATAL_ERROR
            "engine_registry.hpp does not freeze kParakeetPinnedCommit = \"${manifest_commit}\" "
            "as required by native-dependencies.json")
    endif()
    set(VOICETYPER_PARAKEET_ABI_VERSION "${manifest_abi}" CACHE INTERNAL
        "Parakeet C ABI version required by the loader and asserted at runtime")
    set(VOICETYPER_PARAKEET_PINNED_COMMIT "${manifest_commit}" CACHE INTERNAL
        "Parakeet.cpp commit required by the shipped DLL")
    message(STATUS
        "native dependencies: parakeet ABI ${manifest_abi} / commit ${manifest_commit} "
        "agree with engine_registry.hpp")
endfunction()

# --- whisper.cpp -------------------------------------------------------------

# Fetches the pinned whisper.cpp archive, re-verifies its content against the
# manifest, forces the reproducible option set and exposes the result as the
# target `voicetyper_whisper_native` (an INTERFACE target that carries the
# upstream headers, the static whisper/ggml libraries and the compile-time pin
# definitions used by the contract test).
function(voicetyper_fetch_whisper_cpp)
    voicetyper_native_manifest_get(whisper.cpp pinned_commit pinnedCommit)
    voicetyper_native_manifest_get(whisper.cpp repository repository)
    voicetyper_native_manifest_get(whisper.cpp upstream_version upstreamVersion)
    voicetyper_native_manifest_get(whisper.cpp license license)
    voicetyper_native_manifest_get(whisper.cpp archive_url integration archive url)
    voicetyper_native_manifest_get(whisper.cpp archive_sha256 integration archive sha256)
    voicetyper_native_manifest_get(whisper.cpp header_sha256 integration contentPins include/whisper.h)
    voicetyper_native_manifest_get(whisper.cpp implementation_sha256 integration contentPins src/whisper.cpp)
    voicetyper_native_manifest_get(whisper.cpp ggml_cmake_sha256 integration contentPins ggml/CMakeLists.txt)

    if(NOT archive_url MATCHES "${pinned_commit}")
        message(FATAL_ERROR
            "native-dependencies.json: the whisper.cpp archive URL does not contain the pinned "
            "commit ${pinned_commit} (${archive_url})")
    endif()
    # Note: this uses string(REGEX) rather than if(... MATCHES ...) because a
    # brace inside an unquoted if() argument is parsed as a variable reference.
    string(REGEX MATCH "^[0-9a-f]+$" commit_shape "${pinned_commit}")
    string(LENGTH "${pinned_commit}" commit_length)
    if(NOT commit_shape STREQUAL pinned_commit OR NOT commit_length EQUAL 40)
        message(FATAL_ERROR
            "native-dependencies.json: whisper.cpp pinnedCommit '${pinned_commit}' is not a full 40-character commit id")
    endif()

    # The portable registry header freezes the Parakeet pin independently of
    # this module; voicetyper_verify_native_pins() below cross-checks the two at
    # configure time, on every configuration, with or without ASR.

    # Reproducible option set. Everything that would add a network, a GPU, a
    # runtime loader, examples, tests or host-specific codegen is off, and the
    # result is a static archive linked into the VoiceTyper binary.
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(WHISPER_BUILD_SERVER OFF CACHE BOOL "" FORCE)
    set(WHISPER_BUILD_IS_DEV OFF CACHE BOOL "" FORCE)
    set(WHISPER_CURL OFF CACHE BOOL "" FORCE)
    set(WHISPER_SDL2 OFF CACHE BOOL "" FORCE)
    set(WHISPER_USE_SYSTEM_GGML OFF CACHE BOOL "" FORCE)
    set(WHISPER_ALL_WARNINGS OFF CACHE BOOL "" FORCE)
    set(WHISPER_ALL_WARNINGS_3RD_PARTY OFF CACHE BOOL "" FORCE)
    set(WHISPER_SANITIZE_ADDRESS OFF CACHE BOOL "" FORCE)
    set(WHISPER_SANITIZE_UNDEFINED OFF CACHE BOOL "" FORCE)
    set(WHISPER_SANITIZE_THREAD OFF CACHE BOOL "" FORCE)
    set(GGML_NATIVE OFF CACHE BOOL "" FORCE)
    set(GGML_OPENMP OFF CACHE BOOL "" FORCE)
    set(GGML_LTO OFF CACHE BOOL "" FORCE)
    set(GGML_CCACHE OFF CACHE BOOL "" FORCE)
    set(GGML_BACKEND_DL OFF CACHE BOOL "" FORCE)
    set(GGML_ACCELERATE OFF CACHE BOOL "" FORCE)
    set(GGML_BLAS OFF CACHE BOOL "" FORCE)
    set(GGML_RPC OFF CACHE BOOL "" FORCE)
    set(GGML_CUDA OFF CACHE BOOL "" FORCE)
    set(GGML_VULKAN OFF CACHE BOOL "" FORCE)
    set(GGML_METAL OFF CACHE BOOL "" FORCE)
    set(GGML_KOMPUTE OFF CACHE BOOL "" FORCE)
    set(GGML_SYCL OFF CACHE BOOL "" FORCE)
    set(GGML_WEBGPU OFF CACHE BOOL "" FORCE)
    set(GGML_WASM_SINGLE_FILE OFF CACHE BOOL "" FORCE)
    # The CPU backend keeps its runtime dispatch off too: a single generic
    # x86-64 baseline build is the portable, reproducible choice. Optimized
    # per-host kernels are the CPU-dispatch task (t_2d453cc0fd84), not this one.
    set(GGML_CPU_ALL_VARIANTS OFF CACHE BOOL "" FORCE)

    # Pin the x86-64 baseline explicitly, because relying on ggml's default
    # silently produced a *plain SSE2* Windows build.
    #
    # ggml enables SSE4.2/AVX/AVX2/FMA/F16C/BMI2 by default only when
    # GGML_NATIVE_DEFAULT is ON, and upstream sets that to OFF as soon as
    # CMAKE_CROSSCOMPILING is true (for the try_run-based ISA probes). CMake
    # reports CMAKE_CROSSCOMPILING=TRUE for *every* build that uses a toolchain
    # file, and this project's MinGW toolchain is one, so on a native Windows
    # host with CMAKE_HOST_SYSTEM_NAME == CMAKE_SYSTEM_NAME == "Windows" the
    # flag was still TRUE and the vendored ggml compiled for SSE2 only.
    #
    # Measured 2026-10-01 on the target machine with the installed
    # ggml-small-q8_0.bin, same pinned whisper.cpp, same 2.000 s fixture:
    #   SSE2 Windows build: load 577 ms, deep warm-up 145 637 ms, transcribe 62 946 ms
    #   Arch baseline     : load 1 801 ms, deep warm-up  24 045 ms, transcribe 15 046 ms
    # A six-fold difference is not a measurement artifact; it is the missing
    # kernels. The baseline is therefore set here, identically on every host,
    # and host tuning stays off (GGML_NATIVE=OFF, no AVX-512): the compiler may
    # use SSE4.2/AVX/AVX2/FMA/F16C/BMI2, and ggml's own runtime dispatch still
    # selects the kernels at run time.
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64|X86_64)$")
        set(VOICETYPER_GGML_X86_BASELINE
            GGML_SSE42 GGML_AVX GGML_AVX2 GGML_FMA GGML_F16C GGML_BMI2)
        foreach(isa_option IN LISTS VOICETYPER_GGML_X86_BASELINE)
            set(${isa_option} ON CACHE BOOL "" FORCE)
        endforeach()
        foreach(isa_option GGML_AVX512 GGML_AVX_VNNI GGML_AVX512_VBMI GGML_AVX512_VNNI
                            GGML_AVX512_BF16)
            set(${isa_option} OFF CACHE BOOL "" FORCE)
        endforeach()
        message(STATUS
            "whisper.cpp: pinned the generic x86-64 baseline (SSE4.2/AVX/AVX2/FMA/F16C/BMI2, "
            "no AVX-512, no host tuning); ggml's own default drops to SSE2 under "
            "CMAKE_CROSSCOMPILING=${CMAKE_CROSSCOMPILING}")
    endif()

    include(FetchContent)
    # The pinned archive is content-addressed, so there is nothing to update:
    # once populated, re-configuring never re-downloads and never re-resolves a
    # moving tag.
    set(FETCHCONTENT_UPDATES_DISCONNECTED ON CACHE BOOL "" FORCE)
    set(FETCHCONTENT_QUIET OFF CACHE BOOL "" FORCE)

    # Offline escape hatch. A build machine without a usable CA store (a Windows
    # curl that cannot verify github.com, an air-gapped CI runner) can hand the
    # build a pre-fetched whisper.cpp tree instead of downloading the archive.
    # This is not a weaker contract: the content pins below are still verified
    # against the manifest, so a pre-fetched tree is accepted only when it is
    # byte-identical to the pinned commit.
    set(VOICETYPER_WHISPER_PREFETCHED_DIR "" CACHE PATH
        "Pre-fetched whisper.cpp tree for builds without network access; its content is still verified against the manifest pins")
    if(VOICETYPER_WHISPER_PREFETCHED_DIR)
        if(NOT IS_DIRECTORY "${VOICETYPER_WHISPER_PREFETCHED_DIR}")
            message(FATAL_ERROR
                "VOICETYPER_WHISPER_PREFETCHED_DIR is set but is not a directory: ${VOICETYPER_WHISPER_PREFETCHED_DIR}")
        endif()
        message(STATUS
            "whisper.cpp: using the pre-fetched tree ${VOICETYPER_WHISPER_PREFETCHED_DIR} "
            "(no download); the manifest content pins are still verified")
        # EXCLUDE_FROM_ALL (CMake 3.28+, which this project requires) does two things
        # that matter here: the subdirectory's install rules are ignored when the
        # parent is installed, and the parent's install script does not include the
        # subdirectory's one. whisper.cpp 1.9.4 declares install(TARGETS parakeet ...)
        # for a target that is not part of our build graph, so a full
        # `cmake --install` otherwise fails on the missing libparakeet.a. The targets
        # we do use (whisper, ggml) are still built: inter-target dependencies
        # supersede the exclusion.
        FetchContent_Declare(whisper_cpp SOURCE_DIR "${VOICETYPER_WHISPER_PREFETCHED_DIR}"
            EXCLUDE_FROM_ALL)
    else()
        FetchContent_Declare(whisper_cpp
            URL ${archive_url}
            URL_HASH SHA256=${archive_sha256}
            DOWNLOAD_EXTRACT_TIMESTAMP ON
            # See the pre-fetched branch above.
            EXCLUDE_FROM_ALL
        )
    endif()
    FetchContent_MakeAvailable(whisper_cpp)

    if(NOT TARGET whisper)
        message(FATAL_ERROR
            "whisper.cpp ${pinned_commit} was fetched but did not define the 'whisper' target")
    endif()

    # Documented toolchain deviation: MinGW-w64 13.1 does not ship the
    # THREAD_POWER_THROTTLING_STATE type that ggml-cpu.c uses inside
    # `#if _WIN32_WINNT >= 0x0602`, so the pinned whisper.cpp cannot be compiled
    # by the pinned MinGW13 toolchain as-is (verified: the type is absent from
    # mingw1310_64's processthreadsapi.h and present in the newer WinLibs one).
    # Pinning the vendored sub-build to the Windows 7 API level compiles that
    # block out. Consequence: only ggml's "ask Windows not to throttle this
    # thread" optimisation is skipped - a possible throughput difference on
    # Windows 11 with more than four threads, not a correctness change. The
    # alternative (compiling the ASR archive with a different GCC than the app)
    # would mix libstdc++ ABIs, so it was rejected.
    set(VOICETYPER_WHISPER_WINDOWS_API_LEVEL "0x0601" CACHE STRING
        "_WIN32_WINNT used for the vendored whisper.cpp/ggml sub-build (0x0601 works around the MinGW13 THREAD_POWER_THROTTLING_STATE gap; 'default' leaves upstream's choice)")
    if(VOICETYPER_WHISPER_WINDOWS_API_LEVEL STREQUAL "default")
        message(STATUS
            "whisper.cpp: _WIN32_WINNT left at the toolchain default for the vendored sub-build")
    else()
        foreach(whisper_vendored_target ggml ggml-base ggml-cpu whisper parakeet)
            if(TARGET ${whisper_vendored_target})
                target_compile_definitions(${whisper_vendored_target} PRIVATE
                    _WIN32_WINNT=${VOICETYPER_WHISPER_WINDOWS_API_LEVEL}
                    WINVER=${VOICETYPER_WHISPER_WINDOWS_API_LEVEL}
                )
            endif()
        endforeach()
        message(STATUS
            "whisper.cpp: vendored sub-build pinned to _WIN32_WINNT=${VOICETYPER_WHISPER_WINDOWS_API_LEVEL} "
            "(MinGW13 lacks THREAD_POWER_THROTTLING_STATE; only thread power-throttling is skipped)")
    endif()

    # The archive hash proves the download; these three prove the *tree* is the
    # pinned commit (their hashes were taken from an independent git checkout of
    # the same commit, see native-dependencies.json).
    foreach(pinned_file "include/whisper.h:${header_sha256}"
                        "src/whisper.cpp:${implementation_sha256}"
                        "ggml/CMakeLists.txt:${ggml_cmake_sha256}")
        string(REPLACE ":" ";" pinned_parts "${pinned_file}")
        list(GET pinned_parts 0 pinned_relative)
        list(GET pinned_parts 1 pinned_expected)
        set(pinned_absolute "${whisper_cpp_SOURCE_DIR}/${pinned_relative}")
        if(NOT EXISTS "${pinned_absolute}")
            message(FATAL_ERROR
                "whisper.cpp archive is incomplete: ${pinned_relative} is missing")
        endif()
        file(SHA256 "${pinned_absolute}" pinned_actual)
        if(NOT pinned_actual STREQUAL pinned_expected)
            message(FATAL_ERROR
                "whisper.cpp ${pinned_relative} SHA-256 is ${pinned_actual}, the manifest expects "
                "${pinned_expected}.\nThe fetched archive does not match the pinned commit; refusing to build.")
        endif()
    endforeach()

    add_library(voicetyper_whisper_native INTERFACE)
    target_link_libraries(voicetyper_whisper_native INTERFACE whisper)
    target_compile_definitions(voicetyper_whisper_native INTERFACE
        VOICETYPER_WHISPER_PINNED_COMMIT="${pinned_commit}"
        VOICETYPER_WHISPER_UPSTREAM_VERSION="${upstream_version}"
        VOICETYPER_WHISPER_REPOSITORY="${repository}"
        VOICETYPER_WHISPER_LICENSE="${license}"
        VOICETYPER_WHISPER_ARCHIVE_SHA256="${archive_sha256}"
    )

    # Reported at configure time and re-checked by ctest, so the answer to
    # "which whisper.cpp is this build?" is a greppable line, not an assumption.
    set(VOICETYPER_WHISPER_PINNED_COMMIT "${pinned_commit}" CACHE INTERNAL
        "Resolved whisper.cpp pin (also passed to the contract test)")
    set(VOICETYPER_WHISPER_ARCHIVE_SHA256 "${archive_sha256}" CACHE INTERNAL
        "SHA-256 of the fetched whisper.cpp archive (also passed to the contract test)")
    set(VOICETYPER_WHISPER_UPSTREAM_VERSION "${upstream_version}" CACHE INTERNAL
        "Upstream whisper.cpp version string of the pinned commit")
    set(VOICETYPER_WHISPER_SOURCE_DIR "${whisper_cpp_SOURCE_DIR}" CACHE INTERNAL
        "FetchContent source directory of the pinned whisper.cpp archive")
    message(STATUS
        "whisper.cpp: pinned ${pinned_commit} (upstream ${upstream_version}, ${license}), "
        "static, no examples/tests/server, GGML_NATIVE=OFF, OpenMP/WASM/accelerators off; "
        "archive sha256 verified")
    message(STATUS
        "whisper.cpp: content pins verified (include/whisper.h, src/whisper.cpp, ggml/CMakeLists.txt)")
endfunction()
