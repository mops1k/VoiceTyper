# Pins of the vendored llama.cpp runtime (VoiceTyper local text-cleanup model).
#
# This file is the single source of truth for "which llama.cpp is this build
# made of". It is read twice:
#   * at configure time by cmake/LlmRuntime.cmake (the main project), and
#   * at build time by native/llm/CMakeLists.txt (the wrapper project that the
#     ExternalProject configures in its own CMake tree).
# Both read the same constants, so the pin cannot drift between the two.
#
# What is verified, and when:
#   * the archive URL embeds the pinned commit and the commit has the shape of a
#     full 40-character id (configure time, voicetyper_llm_verify_llama_pin_shape);
#   * the downloaded archive is byte-exact: ExternalProject URL_HASH
#     SHA256=<archive sha256> (download time);
#   * the extracted tree really is that commit: the content pins below are
#     re-hashed after extraction (build time,
#     voicetyper_llm_verify_llama_pins). A mismatch is a FATAL_ERROR with the
#     observed hash printed, never a silently accepted tree - the same policy
#     cmake/NativeAsr.cmake applies to whisper.cpp.
#
# The content hashes were taken from the extracted archive of the pinned commit
# itself and are recorded in native/llm/BUILD.txt together with the sizes.

set(VOICETYPER_LLAMA_REPOSITORY "https://github.com/ggml-org/llama.cpp")
set(VOICETYPER_LLAMA_PINNED_COMMIT "d81235049384534c167caea52b85a694f6103d14")
set(VOICETYPER_LLAMA_PINNED_TAG "v0.6.0")
set(VOICETYPER_LLAMA_PINNED_DATE "2026-10-05")
set(VOICETYPER_LLAMA_UPSTREAM_VERSION "0.6.0")
# The ggml llama.cpp vendors at this commit. Recorded because it is the reason
# the runtime is a separate shared library: whisper.cpp pins ggml 0.25.1 and the
# two must never share one set of ggml symbols (see native/llm/BUILD.txt).
set(VOICETYPER_LLAMA_GGML_VERSION "0.26.0")
set(VOICETYPER_LLAMA_LICENSE "MIT")
set(VOICETYPER_LLAMA_LICENSE_HOLDER "Copyright (c) 2023-2026 The ggml authors")
set(VOICETYPER_LLAMA_ARCHIVE_URL
    "${VOICETYPER_LLAMA_REPOSITORY}/archive/${VOICETYPER_LLAMA_PINNED_COMMIT}.tar.gz")
set(VOICETYPER_LLAMA_ARCHIVE_BYTES 37887730)
set(VOICETYPER_LLAMA_ARCHIVE_SHA256
    "6b58785f0a82898f4c3442417ff962e2d4b231b1bee5d033902ad90b27901e14")
# Top-level directory the GitHub commit archive extracts to.
set(VOICETYPER_LLAMA_ARCHIVE_TOP_LEVEL "llama.cpp-${VOICETYPER_LLAMA_PINNED_COMMIT}")

# <relative path>=<sha256>, hashed inside the extracted pinned tree.
set(VOICETYPER_LLAMA_CONTENT_PINS
    "include/llama.h=c9a5f025ad7dbf07eda6660c8ff2c32cd80c07ba440d80728707a9c9d13c1bc3"
    "ggml/include/ggml.h=12ee71f99db7db9b353bc02b1fbb57c344ee17c01ac5fb7952b41a637a747ea9"
    "ggml/src/ggml.c=7e8bf2b60598b50d718f46aa3c36c9293b10f4e7dfde6844f0693e6fc976d505"
    "ggml/src/ggml-cpu/ggml-cpu.c=45b22f8c83b2cbccabfc2f81a38ef8a5810223e1ce366b7c6f77d42faa5cc6f3"
    "CMakeLists.txt=94d646f48594469909522d2a539aad943705309686646fd8a69cc15c43029e69"
    "ggml/CMakeLists.txt=c1d1d55416664981ea61e33b28a0dfdd36064659f15661d3a500b0de98d59ce4"
)

# The llama.cpp source directory the wrapper project compiles against. Set by
# cmake/LlmRuntime.cmake (-DVOICETYPER_LLAMA_SOURCE_DIR=...) to either the
# verified download or a pre-fetched tree on an offline build machine.
set(VOICETYPER_LLAMA_SOURCE_DIR "" CACHE PATH
    "Pre-fetched llama.cpp tree; its content is verified against the pins in native/llm/LlamaPins.cmake")

# Fails configure/build when the pin constants contradict themselves. Cheap, and
# it catches a half-edited pin before any download happens.
function(voicetyper_llm_verify_llama_pin_shape)
    if(NOT VOICETYPER_LLAMA_ARCHIVE_URL MATCHES "${VOICETYPER_LLAMA_PINNED_COMMIT}")
        message(FATAL_ERROR
            "LlamaPins.cmake: the llama.cpp archive URL does not contain the pinned commit "
            "${VOICETYPER_LLAMA_PINNED_COMMIT} (${VOICETYPER_LLAMA_ARCHIVE_URL})")
    endif()
    # string(REGEX) rather than if(... MATCHES ...): a brace in an unquoted if()
    # argument is parsed as a variable reference.
    string(REGEX MATCH "^[0-9a-f]+$" commit_shape "${VOICETYPER_LLAMA_PINNED_COMMIT}")
    string(LENGTH "${VOICETYPER_LLAMA_PINNED_COMMIT}" commit_length)
    if(NOT commit_shape STREQUAL VOICETYPER_LLAMA_PINNED_COMMIT OR NOT commit_length EQUAL 40)
        message(FATAL_ERROR
            "LlamaPins.cmake: llama.cpp pinned commit '${VOICETYPER_LLAMA_PINNED_COMMIT}' "
            "is not a full 40-character commit id")
    endif()
    if(NOT VOICETYPER_LLAMA_ARCHIVE_SHA256 MATCHES "^[0-9a-f]+$")
        message(FATAL_ERROR "LlamaPins.cmake: llama.cpp archive SHA-256 is not a hex string")
    endif()
endfunction()

# Re-hashes every content pin inside `source_dir`. The archive hash proves the
# download; these prove the *tree* is the pinned commit, which is what makes a
# pre-fetched directory acceptable offline without weakening the contract.
function(voicetyper_llm_verify_llama_pins source_dir)
    if(NOT IS_DIRECTORY "${source_dir}")
        message(FATAL_ERROR
            "llama.cpp: the source directory ${source_dir} does not exist; nothing to verify")
    endif()
    foreach(pinned_entry IN LISTS VOICETYPER_LLAMA_CONTENT_PINS)
        string(REPLACE "=" ";" pinned_parts "${pinned_entry}")
        list(GET pinned_parts 0 pinned_relative)
        list(GET pinned_parts 1 pinned_expected)
        set(pinned_absolute "${source_dir}/${pinned_relative}")
        if(NOT EXISTS "${pinned_absolute}")
            message(FATAL_ERROR
                "llama.cpp archive is incomplete: ${pinned_relative} is missing from ${source_dir}")
        endif()
        file(SHA256 "${pinned_absolute}" pinned_actual)
        if(NOT pinned_actual STREQUAL pinned_expected)
            message(FATAL_ERROR
                "llama.cpp ${pinned_relative} SHA-256 is ${pinned_actual}, the pin expects "
                "${pinned_expected}.\nThe tree is not the pinned commit "
                "${VOICETYPER_LLAMA_PINNED_COMMIT}; refusing to build.")
        endif()
    endforeach()
    list(LENGTH VOICETYPER_LLAMA_CONTENT_PINS pinned_count)
    message(STATUS
        "llama.cpp: content pins verified (${pinned_count} files, commit ${VOICETYPER_LLAMA_PINNED_COMMIT})")
endfunction()
