cmake_minimum_required(VERSION 3.28)

# Wrapper project of the VoiceTyper local text-cleanup runtime.
#
# This is NOT configured as part of the main VoiceTyper project: it is run by an
# ExternalProject (cmake/LlmRuntime.cmake) in its own CMake tree, for the same
# reason parakeet.cpp and transcribe.cpp are built that way - llama.cpp declares
# the targets `ggml`, `ggml-base` and `ggml-cpu` unconditionally, whisper.cpp
# (which the application links statically) declares the very same names, and one
# CMake project cannot hold both. A separate tree also keeps llama.cpp's ggml
# 0.26.0 out of the application binary, where whisper.cpp's ggml 0.25.1 lives.
#
# The product of this project is exactly one shared library,
# voicetyper_llm.so / voicetyper_llm.dll, with llama.cpp and its ggml linked
# statically inside it. Only native/llm/voicetyper_llm.h crosses the boundary.

project(VoiceTyperLlmRuntime LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/LlamaPins.cmake")
voicetyper_llm_verify_llama_pin_shape()

if(NOT VOICETYPER_LLAMA_SOURCE_DIR)
    message(FATAL_ERROR
        "VoiceTyperLlmRuntime: VOICETYPER_LLAMA_SOURCE_DIR is empty. "
        "cmake/LlmRuntime.cmake must pass the verified llama.cpp tree")
endif()
if(NOT EXISTS "${VOICETYPER_LLAMA_SOURCE_DIR}/CMakeLists.txt")
    # The commit archive extracts to a single top-level directory
    # (llama.cpp-<commit>); accept either that directory itself or its parent so
    # the caller does not have to know how ExternalProject laid the tree out.
    file(GLOB llama_candidates "${VOICETYPER_LLAMA_SOURCE_DIR}/*/CMakeLists.txt")
    list(LENGTH llama_candidates llama_candidate_count)
    if(llama_candidate_count EQUAL 1)
        get_filename_component(VOICETYPER_LLAMA_SOURCE_DIR "${llama_candidates}" DIRECTORY)
    endif()
endif()
if(NOT EXISTS "${VOICETYPER_LLAMA_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "VoiceTyperLlmRuntime: ${VOICETYPER_LLAMA_SOURCE_DIR} does not look like a llama.cpp tree")
endif()
# The same content check the main project documents: the archive hash proves the
# download, these hashes prove the tree is the pinned commit.
voicetyper_llm_verify_llama_pins("${VOICETYPER_LLAMA_SOURCE_DIR}")

message(STATUS
    "llama.cpp: pinned ${VOICETYPER_LLAMA_PINNED_COMMIT} (${VOICETYPER_LLAMA_PINNED_TAG}, upstream "
    "${VOICETYPER_LLAMA_UPSTREAM_VERSION}, ggml ${VOICETYPER_LLAMA_GGML_VERSION}, "
    "${VOICETYPER_LLAMA_LICENSE}); archive sha256 verified, static, CPU only, ggml linked into "
    "voicetyper_llm")

# --- llama.cpp options --------------------------------------------------------
# Reproducible, offline, CPU-only set. Everything that would pull a network
# client, a GPU backend, a runtime loader, examples, tests or host-specific
# codegen is off, and llama.cpp + ggml are static archives that end up inside
# voicetyper_llm.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_IS_DEV OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_COMMON OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_SERVER OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_APP OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_UI OFF CACHE BOOL "" FORCE)
set(LLAMA_USE_PREBUILT_UI OFF CACHE BOOL "" FORCE)
set(LLAMA_CURL OFF CACHE BOOL "" FORCE)
set(LLAMA_OPENSSL OFF CACHE BOOL "" FORCE)
set(LLAMA_SUBPROCESS OFF CACHE BOOL "" FORCE)
set(LLAMA_ALL_WARNINGS OFF CACHE BOOL "" FORCE)
set(LLAMA_ALL_WARNINGS_3RD_PARTY OFF CACHE BOOL "" FORCE)
set(LLAMA_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
set(LLAMA_SANITIZE_ADDRESS OFF CACHE BOOL "" FORCE)
set(LLAMA_SANITIZE_UNDEFINED OFF CACHE BOOL "" FORCE)
set(LLAMA_SANITIZE_THREAD OFF CACHE BOOL "" FORCE)
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
set(GGML_CPU_ALL_VARIANTS OFF CACHE BOOL "" FORCE)
set(GGML_CPU_KLEIDIAI OFF CACHE BOOL "" FORCE)

# Same reasoned choice cmake/NativeAsr.cmake makes for whisper.cpp: pin the
# generic x86-64 baseline instead of letting ggml fall back to plain SSE2 under
# CMAKE_CROSSCOMPILING (which CMake reports for every build that uses a toolchain
# file, i.e. the whole Windows MinGW build). ggml's own runtime dispatch still
# picks the kernels at run time; host tuning and AVX-512 stay off so one binary
# runs everywhere.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64|X86_64)$")
    foreach(isa_option GGML_SSE42 GGML_AVX GGML_AVX2 GGML_FMA GGML_F16C GGML_BMI2)
        set(${isa_option} ON CACHE BOOL "" FORCE)
    endforeach()
    foreach(isa_option GGML_AVX512 GGML_AVX_VNNI GGML_AVX512_VBMI GGML_AVX512_VNNI GGML_AVX512_BF16)
        set(${isa_option} OFF CACHE BOOL "" FORCE)
    endforeach()
endif()

# Static code linked into a shared object has to be position independent on
# ELF; harmless on Windows, where the DLL is one relocation unit anyway.
set(CMAKE_POSITION_INDEPENDENT_CODE ON CACHE BOOL "" FORCE)

add_subdirectory("${VOICETYPER_LLAMA_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/llama.cpp" EXCLUDE_FROM_ALL)

# MinGW-w64 13.1 (the Qt toolchain the product ships with) does not declare
# THREAD_POWER_THROTTLING_STATE, which ggml-cpu.c uses inside
# `#if _WIN32_WINNT >= 0x0602`; pinning the vendored sub-build to the Windows 7
# API level compiles that block out. Identical deviation to the whisper.cpp one,
# with the same consequence: only ggml's "ask Windows not to throttle this
# thread" optimisation is skipped.
if(WIN32)
    foreach(llama_vendored_target ggml ggml-base ggml-cpu llama)
        if(TARGET ${llama_vendored_target})
            target_compile_definitions(${llama_vendored_target} PRIVATE
                _WIN32_WINNT=0x0601
                WINVER=0x0601
            )
        endif()
    endforeach()
    message(STATUS
        "llama.cpp: vendored sub-build pinned to _WIN32_WINNT=0x0601 "
        "(MinGW13 lacks THREAD_POWER_THROTTLING_STATE)")
endif()

if(NOT TARGET llama)
    message(FATAL_ERROR
        "llama.cpp ${VOICETYPER_LLAMA_PINNED_COMMIT} was configured but did not define the 'llama' target")
endif()

# --- the VoiceTyper library ---------------------------------------------------
add_library(voicetyper_llm SHARED
    voicetyper_llm.cpp
)
target_include_directories(voicetyper_llm PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_compile_definitions(voicetyper_llm PRIVATE
    VOICETYPER_LLM_BUILD=1
    VOICETYPER_LLAMA_PINNED_COMMIT="${VOICETYPER_LLAMA_PINNED_COMMIT}"
    VOICETYPER_LLAMA_UPSTREAM_VERSION="${VOICETYPER_LLAMA_UPSTREAM_VERSION}"
    VOICETYPER_LLAMA_GGML_VERSION="${VOICETYPER_LLAMA_GGML_VERSION}"
    VOICETYPER_LLAMA_REPOSITORY="${VOICETYPER_LLAMA_REPOSITORY}"
    VOICETYPER_LLAMA_LICENSE="${VOICETYPER_LLAMA_LICENSE}"
    VOICETYPER_LLAMA_ARCHIVE_SHA256="${VOICETYPER_LLAMA_ARCHIVE_SHA256}"
)
target_link_libraries(voicetyper_llm PRIVATE llama)
if(MSVC)
    target_compile_options(voicetyper_llm PRIVATE /W4 /permissive-)
else()
    target_compile_options(voicetyper_llm PRIVATE -Wall -Wextra -Wpedantic)
endif()
set_target_properties(voicetyper_llm PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    PREFIX ""
    OUTPUT_NAME "voicetyper_llm"
    # Only voicetyper_llm.h is the public surface; nothing of llama.cpp or ggml
    # may become a dynamic symbol of this library.
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

if(WIN32)
    # A leaf DLL: no other MinGW runtime DLL has to be shipped or loaded for it
    # (contrast native/transcribe/BUILD.txt, where the library links other MinGW
    # DLLs and therefore cannot be static).
    target_link_options(voicetyper_llm PRIVATE -static -static-libgcc -static-libstdc++)
else()
    # Hides every symbol that came out of the static llama/ggml archives, so the
    # dynamic symbol table of the shared object is our ABI and nothing else.
    target_link_options(voicetyper_llm PRIVATE "-Wl,--exclude-libs,ALL")
endif()

install(TARGETS voicetyper_llm
    RUNTIME DESTINATION .
    LIBRARY DESTINATION .
    ARCHIVE DESTINATION .
)
