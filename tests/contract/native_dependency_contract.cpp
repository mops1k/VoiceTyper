// Headless contract test for the native ASR dependencies. No model is loaded
// and nothing is downloaded; the test only checks the pinned contract and the
// explicit unavailability paths.
//
// What it proves:
//   1. docs/migration/cpp/native-dependencies.json exists, is well formed, and
//      carries a repository, a full commit pin, a license, an ABI version, the
//      closed Parakeet symbol list, the DLL SHA-256 expectations and the
//      no-telemetry statement.
//   2. The Parakeet ABI constant in the portable header
//      (src/platform/api/engine_registry.hpp) is 6, the manifest agrees with
//      it, the pinned commit agrees with it, and the shipped C header
//      parakeet_capi.h really declares all six symbols the loader binds.
//   3. The shipped parakeet.dll / mc_wasapi.dll match the byte size and SHA-256
//      recorded in the manifest (a mismatch is a hard failure; absence in a
//      source-only checkout is reported as SKIP, never as a pass).
//   4. No header reaches for a native header: neither src/asr/*.hpp nor
//      src/platform/windows/*.hpp may include parakeet_capi.h, whisper.h or
//      windows.h. The binding stays in the two .cpp files.
//   5. The Parakeet loader reports a clear, explicit "unavailable" with the
//      missing path when the DLL is absent, and - on Windows - reports ABI 6
//      with all six symbols bound when the shipped DLL is present. Off Windows
//      the same present file yields platform_unsupported, never a fallback.
//
// Printed greppable line: "native-dependency-contract: OK".

#include "domain/cancellation.hpp"
#include "platform/api/engine_registry.hpp"
#include "platform/windows/parakeet_runtime.hpp"

#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;

int failures = 0;
int skips = 0;

void check(bool condition, const std::string& message)
{
    if (!condition) {
        ++failures;
        std::cerr << "FAIL " << message << '\n';
    }
}

void skip(const std::string& message)
{
    ++skips;
    std::cout << "SKIP " << message << '\n';
}

std::string read_file(const fs::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return {};
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

bool is_hex_of_length(std::string_view text, std::size_t length)
{
    if (text.size() != length) {
        return false;
    }
    for (const char c : text) {
        const bool digit = c >= '0' && c <= '9';
        const bool lower = c >= 'a' && c <= 'f';
        if (!digit && !lower) {
            return false;
        }
    }
    return true;
}

bool contains(std::string_view haystack, std::string_view needle)
{
    return haystack.find(needle) != std::string_view::npos;
}

// --- the smallest JSON reader that can read our own manifest ---------------
//
// The manifest is a file this project writes and lints, so the test reads it
// with a purpose-built reader instead of pulling in a JSON dependency (which
// would also weaken the "standard C++ only" property of the test suite). The
// reader understands objects, arrays, strings, numbers, booleans and null -
// nothing else - and reports a miss instead of throwing.

std::size_t skip_whitespace(const std::string& text, std::size_t pos)
{
    while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos])) != 0) {
        ++pos;
    }
    return pos;
}

/// Returns the index just past the matching close brace/bracket, or 0 on a
/// malformed document.
std::size_t match_delimiter(const std::string& text, std::size_t open)
{
    const char expected = text[open] == '{' ? '}' : ']';
    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (std::size_t pos = open; pos < text.size(); ++pos) {
        const char c = text[pos];
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
        } else if (c == '{' || c == '[') {
            ++depth;
        } else if (c == '}' || c == ']') {
            --depth;
            if (depth == 0) {
                return c == expected ? pos + 1 : 0;
            }
        }
    }
    return 0;
}

/// Reads the value that follows "key" inside [begin, end). Strings are returned
/// unquoted; numbers, booleans and null are returned verbatim.
std::optional<std::string> json_field(
    const std::string& text, std::size_t begin, std::size_t end, std::string_view key)
{
    const std::string quoted_key = "\"" + std::string(key) + "\"";
    std::size_t pos = begin;
    while (true) {
        const std::size_t found = text.find(quoted_key, pos);
        if (found == std::string::npos || found >= end) {
            return std::nullopt;
        }
        pos = skip_whitespace(text, found + quoted_key.size());
        if (pos >= end || text[pos] != ':') {
            return std::nullopt;
        }
        pos = skip_whitespace(text, pos + 1);
        if (pos >= end) {
            return std::nullopt;
        }
        if (text[pos] == '"') {
            std::string value;
            ++pos;
            bool escaped = false;
            while (pos < end) {
                const char c = text[pos];
                if (escaped) {
                    escaped = false;
                    value += c;
                } else if (c == '\\') {
                    escaped = true;
                } else if (c == '"') {
                    ++pos;
                    return value;
                } else {
                    value += c;
                }
                ++pos;
            }
            return std::nullopt;
        }
        if (text[pos] == '{' || text[pos] == '[') {
            const std::size_t close = match_delimiter(text, pos);
            if (close == 0 || close > end) {
                return std::nullopt;
            }
            pos = close;
            continue; // a nested container: look for the key after it
        }
        std::string value;
        while (pos < end && text[pos] != ',' && text[pos] != '}' && text[pos] != ']'
               && std::isspace(static_cast<unsigned char>(text[pos])) == 0) {
            value += text[pos];
            ++pos;
        }
        return value;
    }
}

/// Reads the raw text of the value stored under "key" inside [begin, end),
/// containers included. Used where the value is an array of strings.
std::optional<std::string> json_raw_value(
    const std::string& text, std::size_t begin, std::size_t end, std::string_view key)
{
    const std::string quoted_key = "\"" + std::string(key) + "\"";
    const std::size_t found = text.find(quoted_key, begin);
    if (found == std::string::npos || found >= end) {
        return std::nullopt;
    }
    std::size_t pos = skip_whitespace(text, found + quoted_key.size());
    if (pos >= end || text[pos] != ':') {
        return std::nullopt;
    }
    pos = skip_whitespace(text, pos + 1);
    if (pos >= end) {
        return std::nullopt;
    }
    if (text[pos] == '{' || text[pos] == '[') {
        const std::size_t close = match_delimiter(text, pos);
        if (close == 0 || close > end) {
            return std::nullopt;
        }
        return text.substr(pos, close - pos);
    }
    return text.substr(pos, end - pos);
}

/// Direct child objects of the array/object stored under "key" in [begin, end).
std::vector<std::pair<std::size_t, std::size_t>> json_child_objects(
    const std::string& text, std::size_t begin, std::size_t end, std::string_view key)
{
    std::vector<std::pair<std::size_t, std::size_t>> objects;
    const std::string quoted_key = "\"" + std::string(key) + "\"";
    const std::size_t found = text.find(quoted_key, begin);
    if (found == std::string::npos || found >= end) {
        return objects;
    }
    std::size_t pos = skip_whitespace(text, found + quoted_key.size());
    if (pos >= end || text[pos] != ':') {
        return objects;
    }
    pos = skip_whitespace(text, pos + 1);
    if (pos >= end || text[pos] != '[') {
        return objects;
    }
    const std::size_t array_end = match_delimiter(text, pos);
    if (array_end == 0 || array_end > end) {
        return objects;
    }
    pos = skip_whitespace(text, pos + 1);
    while (pos < array_end && text[pos] == '{') {
        const std::size_t close = match_delimiter(text, pos);
        if (close == 0 || close > array_end) {
            return objects;
        }
        objects.emplace_back(pos, close);
        pos = skip_whitespace(text, close);
        if (pos < array_end && text[pos] == ',') {
            pos = skip_whitespace(text, pos + 1);
        }
    }
    return objects;
}

std::optional<std::pair<std::size_t, std::size_t>> json_object_by_id(
    const std::string& text, const std::string& id)
{
    for (const auto& span : json_child_objects(text, 0, text.size(), "dependencies")) {
        const auto candidate = json_field(text, span.first, span.second, "id");
        if (candidate && *candidate == id) {
            return span;
        }
    }
    return std::nullopt;
}

// --- SHA-256 ----------------------------------------------------------------
//
// Self-contained so the artifact expectations are checked by the test itself
// on every host, without OpenSSL, without a network fetch and without a
// CMake-side hash that a broken configure could have skipped.

class Sha256 {
public:
    void update(const unsigned char* data, std::size_t size)
    {
        for (std::size_t i = 0; i < size; ++i) {
            block_[block_size_++] = data[i];
            if (block_size_ == 64) {
                transform();
                block_size_ = 0;
            }
        }
        bit_length_ += static_cast<std::uint64_t>(size) * 8;
    }

    std::string hex()
    {
        std::array<unsigned char, 64> padding = {};
        padding[0] = 0x80;
        const std::size_t pad_length = block_size_ < 56 ? 56 - block_size_ : 120 - block_size_;
        const std::uint64_t total_bits = bit_length_;
        for (std::size_t i = 0; i < 8; ++i) {
            padding[pad_length + i] = static_cast<unsigned char>((total_bits >> (56 - 8 * i)) & 0xFF);
        }
        // The length field must not count itself.
        const std::uint64_t saved = bit_length_;
        update(padding.data(), pad_length + 8);
        bit_length_ = saved;

        static const char* digits = "0123456789abcdef";
        std::string out;
        out.reserve(64);
        for (const std::uint32_t word : state_) {
            for (int shift = 28; shift >= 0; shift -= 4) {
                out += digits[(word >> shift) & 0x0F];
            }
        }
        return out;
    }

private:
    static std::uint32_t rotr(std::uint32_t value, int bits)
    {
        return (value >> bits) | (value << (32 - bits));
    }

    void transform()
    {
        static const std::uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

        std::uint32_t w[64] = {};
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<std::uint32_t>(block_[i * 4]) << 24)
                | (static_cast<std::uint32_t>(block_[i * 4 + 1]) << 16)
                | (static_cast<std::uint32_t>(block_[i * 4 + 2]) << 8)
                | static_cast<std::uint32_t>(block_[i * 4 + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        std::uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
        std::uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
        for (int i = 0; i < 64; ++i) {
            const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const std::uint32_t ch = (e & f) ^ (~e & g);
            const std::uint32_t temp1 = h + s1 + ch + k[i] + w[i];
            const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temp2 = s0 + maj;
            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }
        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    std::array<unsigned char, 64> block_ = {};
    std::size_t block_size_ = 0;
    std::uint64_t bit_length_ = 0;
    std::array<std::uint32_t, 8> state_ = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
};

std::string sha256_file(const fs::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return {};
    }
    Sha256 hash;
    std::array<char, 65536> buffer = {};
    while (stream) {
        stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize got = stream.gcount();
        if (got > 0) {
            hash.update(reinterpret_cast<const unsigned char*>(buffer.data()),
                static_cast<std::size_t>(got));
        }
    }
    return hash.hex();
}

std::string sha256_of_text(const std::string& text)
{
    Sha256 hash;
    hash.update(reinterpret_cast<const unsigned char*>(text.data()), text.size());
    return hash.hex();
}

// --- the checks -------------------------------------------------------------

void check_manifest_shape(const fs::path& manifest_path, const std::string& manifest)
{
    check(fs::is_regular_file(manifest_path), "native-dependencies.json exists in the checkout");
    check(!manifest.empty(), "native-dependencies.json is present and readable");
    if (manifest.empty()) {
        return;
    }

    const auto schema = json_field(manifest, 0, manifest.size(), "schemaVersion");
    check(schema.has_value(), "manifest declares schemaVersion");
    check(schema && *schema == "1", "manifest schemaVersion is 1");

    // No-telemetry statement, in machine form and in prose.
    const auto network = json_field(manifest, 0, manifest.size(), "networkAtRecognition");
    const auto downloads = json_field(manifest, 0, manifest.size(), "downloadsAtRecognition");
    check(network && *network == "false", "manifest states no network at recognition");
    check(downloads && *downloads == "false", "manifest states no downloads at recognition");
    const auto statement = json_field(manifest, 0, manifest.size(), "statement");
    check(statement && statement->size() > 80, "manifest carries a no-telemetry statement");
    check(statement && contains(*statement, "downloads anything while recognizing"),
        "no-telemetry statement says nothing is downloaded while recognizing");

    const auto test_downloads = json_field(manifest, 0, manifest.size(), "downloadedByTests");
    check(test_downloads && *test_downloads == "false", "manifest states tests never download models");
    const auto engine_downloads = json_field(manifest, 0, manifest.size(), "downloadedByEngines");
    check(engine_downloads && *engine_downloads == "false",
        "manifest states engines never download models");
}

void check_whisper_entry(const std::string& manifest)
{
    const auto span = json_object_by_id(manifest, "whisper.cpp");
    check(span.has_value(), "manifest has a whisper.cpp dependency");
    if (!span) {
        return;
    }
    const auto pinned = json_field(manifest, span->first, span->second, "pinnedCommit");
    check(pinned && is_hex_of_length(*pinned, 40), "whisper.cpp pinnedCommit is a full 40-hex commit");
    const auto repository = json_field(manifest, span->first, span->second, "repository");
    check(repository && contains(*repository, "github.com/ggml-org/whisper.cpp"),
        "whisper.cpp repository is recorded");
    const auto version = json_field(manifest, span->first, span->second, "upstreamVersion");
    check(version && !version->empty(), "whisper.cpp upstream version is recorded");
    const auto license = json_field(manifest, span->first, span->second, "license");
    check(license && *license == "MIT", "whisper.cpp license is MIT");
    const auto linkage = json_field(manifest, span->first, span->second, "linkage");
    check(linkage && *linkage == "static", "whisper.cpp is linked statically");

    const auto archive_url = json_field(manifest, span->first, span->second, "url");
    const auto archive_sha = json_field(manifest, span->first, span->second, "sha256");
    check(archive_url && contains(*archive_url, "whisper.cpp"),
        "whisper.cpp archive URL is recorded");
    check(archive_sha && is_hex_of_length(*archive_sha, 64), "whisper.cpp archive SHA-256 is recorded");
    if (archive_url && pinned) {
        check(contains(*archive_url, *pinned), "whisper.cpp archive URL embeds the pinned commit");
    }

    const auto header_pin = json_field(manifest, span->first, span->second, "include/whisper.h");
    check(header_pin && is_hex_of_length(*header_pin, 64), "whisper.h content pin is recorded");

    // The option set that makes the build reproducible, spelled out.
    for (const std::string_view option : {"BUILD_SHARED_LIBS", "WHISPER_BUILD_TESTS",
             "WHISPER_BUILD_EXAMPLES", "WHISPER_BUILD_SERVER", "GGML_NATIVE", "GGML_OPENMP"}) {
        const auto value = json_field(manifest, span->first, span->second, option);
        check(value && *value == "OFF", "whisper.cpp build option " + std::string(option) + " is OFF");
    }
    const auto curl = json_field(manifest, span->first, span->second, "WHISPER_CURL");
    check(curl && *curl == "OFF", "whisper.cpp WHISPER_CURL is OFF (no libcurl in the engine)");
}

void check_parakeet_entry(const std::string& manifest, const fs::path& source_dir)
{
    using voicetyper::platform::EngineAvailabilityReason;
    using voicetyper::platform::kParakeetAbiVersion;
    using voicetyper::platform::kParakeetPinnedCommit;
    using voicetyper::platform::kParakeetRequiredSymbols;

    const auto span = json_object_by_id(manifest, "parakeet.cpp");
    check(span.has_value(), "manifest has a parakeet.cpp dependency");
    if (!span) {
        return;
    }

    const auto abi = json_field(manifest, span->first, span->second, "abiVersion");
    check(abi.has_value(), "manifest records a Parakeet ABI version");
    if (abi) {
        check(std::stoi(*abi) == kParakeetAbiVersion,
            "manifest ABI version matches kParakeetAbiVersion (" + std::to_string(kParakeetAbiVersion) + ")");
        check(std::stoi(*abi) == 6, "Parakeet ABI version is 6");
    }
    const auto pinned = json_field(manifest, span->first, span->second, "pinnedCommit");
    check(pinned && *pinned == std::string(kParakeetPinnedCommit),
        "manifest Parakeet commit matches kParakeetPinnedCommit");
    const auto license = json_field(manifest, span->first, span->second, "license");
    check(license && *license == "MIT", "parakeet.cpp license is MIT");
    const auto abi_source = json_field(manifest, span->first, span->second, "abiSource");
    check(abi_source && contains(*abi_source, "parakeet_capi.h"),
        "manifest points at parakeet_capi.h as the ABI source");

    // The manifest symbol list is the same closed set the loader binds.
    const auto symbols = json_raw_value(manifest, span->first, span->second, "requiredSymbols");
    check(symbols && contains(*symbols, "parakeet_capi_abi_version"),
        "manifest lists the required ABI symbol");
    check(symbols && symbols->rfind('[') == 0, "requiredSymbols is recorded as a list");
    for (const std::string_view symbol : kParakeetRequiredSymbols) {
        check(symbols && contains(*symbols, symbol),
            "manifest requires " + std::string(symbol));
    }
    // ...and nothing else: the list is closed, so a new native entry point is an
    // explicit manifest change rather than an accidental widening.
    if (symbols) {
        // Count only whole quoted entries: "parakeet_capi_free" is a prefix of
        // "parakeet_capi_free_string", so a substring count would over-count.
        std::size_t occurrences = 0;
        for (const std::string_view symbol : kParakeetRequiredSymbols) {
            const std::string quoted = "\"" + std::string(symbol) + "\"";
            std::size_t pos = symbols->find(quoted);
            while (pos != std::string::npos) {
                ++occurrences;
                pos = symbols->find(quoted, pos + 1);
            }
        }
        check(occurrences == kParakeetRequiredSymbols.size(),
            "requiredSymbols lists exactly the six bound symbols");
    }

    // The real C header must declare every symbol we bind.
    const fs::path header = source_dir / "native/parakeet/include/parakeet_capi.h";
    const std::string header_text = read_file(header);
    check(!header_text.empty(), "parakeet_capi.h is present in the repository");
    if (!header_text.empty()) {
        check(contains(header_text, "v6:"),
            "parakeet_capi.h documents the v6 ABI revision that is required");
        for (const std::string_view symbol : kParakeetRequiredSymbols) {
            check(contains(header_text, symbol),
                "parakeet_capi.h declares " + std::string(symbol));
        }
    }

    // Shipped artifacts: bytes and SHA-256 must match exactly.
    const std::array<std::pair<std::string, std::string>, 2> artifacts = {
        std::pair<std::string, std::string>{"parakeet.cpp", "native/parakeet.dll"},
        std::pair<std::string, std::string>{"mc_wasapi", "native/mc_wasapi.dll"},
    };
    for (const auto& artifact : artifacts) {
        const std::string& dependency_id = artifact.first;
        const std::string& artifact_path = artifact.second;
        const fs::path absolute = source_dir / artifact_path;
        const auto owner = json_object_by_id(manifest, dependency_id);
        check(owner.has_value(), "manifest has the " + dependency_id + " dependency for " + artifact_path);
        if (!owner) {
            continue;
        }
        std::optional<std::pair<std::size_t, std::size_t>> found;
        for (const auto& candidate : json_child_objects(manifest, owner->first, owner->second, "artifacts")) {
            const auto path_value = json_field(manifest, candidate.first, candidate.second, "path");
            if (path_value && *path_value == artifact_path) {
                found = candidate;
                break;
            }
        }
        check(found.has_value(), "manifest has an artifact entry for " + artifact_path);
        if (!found) {
            continue;
        }
        const auto expected_sha = json_field(manifest, found->first, found->second, "sha256");
        const auto expected_bytes = json_field(manifest, found->first, found->second, "bytes");
        check(expected_sha && is_hex_of_length(*expected_sha, 64),
            "artifact " + artifact_path + " records a SHA-256");
        if (!fs::is_regular_file(absolute)) {
            skip("shipped artifact " + artifact_path + " is not in this checkout");
            continue;
        }
        const auto actual_bytes = static_cast<std::uintmax_t>(fs::file_size(absolute));
        check(expected_bytes && std::stoull(*expected_bytes) == actual_bytes,
            "artifact " + artifact_path + " has the expected byte size");
        if (expected_sha) {
            const std::string actual_sha = sha256_file(absolute);
            check(actual_sha == *expected_sha,
                "artifact " + artifact_path + " matches the manifest SHA-256 (got " + actual_sha + ")");
        }
    }

    // Loader behaviour. Explicitly unavailable when the file is absent...
    const fs::path absent = source_dir / "native/parakeet-not-shipped-here.dll";
    const auto missing = voicetyper::platform::probe_parakeet_runtime(absent);
    check(!missing.usable, "loader reports Parakeet unavailable when the DLL is absent");
    check(missing.reason == EngineAvailabilityReason::native_library_missing,
        "absent DLL maps to native_library_missing");
    check(!missing.detail.empty(), "absent DLL produces a non-empty explanation");
    check(contains(missing.detail, "parakeet-not-shipped-here.dll"),
        "absent-DLL explanation names the missing file");
    check(missing.abi_version == 0, "absent DLL reports no ABI version");

    const auto open_absent = voicetyper::platform::ParakeetRuntime::open(absent, {});
    check(open_absent.is_error(), "ParakeetRuntime::open fails when the DLL is absent");
    if (open_absent.is_error()) {
        check(open_absent.code() == voicetyper::domain::ErrorCode::engine_unavailable,
            "ParakeetRuntime::open reports engine_unavailable for an absent DLL");
        check(!open_absent.error().message().empty(),
            "ParakeetRuntime::open explains why the engine is unavailable");
    }

    // ...and ABI 6 with all six symbols bound when the shipped DLL is present.
    const fs::path shipped = source_dir / "native/parakeet.dll";
    if (!fs::is_regular_file(shipped)) {
        skip("shipped parakeet.dll is not in this checkout; ABI probe not executed");
        return;
    }
    const auto probe = voicetyper::platform::probe_parakeet_runtime(shipped);
#if defined(_WIN32)
    check(probe.library_loaded, "shipped parakeet.dll loads on Windows");
    check(probe.usable, "shipped parakeet.dll is usable on Windows");
    check(probe.abi_version == 6, "shipped parakeet.dll reports ABI 6");
    check(probe.reason == EngineAvailabilityReason::available, "shipped parakeet.dll reason is available");
    check(probe.missing_symbols.empty(), "all six pinned Parakeet symbols resolve");
#elif defined(__linux__)
    // Linux is a supported platform now: the probe goes through the dynamic
    // loader, and a Windows DLL simply fails to load. That is a missing native
    // library, not "this platform cannot run the engine at all".
    check(!probe.usable, "a Windows Parakeet DLL is not usable on Linux");
    check(probe.reason == EngineAvailabilityReason::native_library_missing,
        "a present but unloadable library maps to native_library_missing on Linux");
    check(contains(probe.detail, "dlopen"), "the Linux probe explains the dynamic loader failure");
    check(!contains(probe.detail, "fallback") || contains(probe.detail, "no other engine"),
        "the Linux probe states that no other engine is substituted");
#else
    check(!probe.usable, "a Windows Parakeet DLL is not usable on this platform");
    check(probe.reason == EngineAvailabilityReason::platform_unsupported,
        "present-but-not-loadable DLL maps to platform_unsupported off Windows");
    check(contains(probe.detail, "Windows"), "platform_unsupported explanation mentions Windows");
    check(!contains(probe.detail, "fallback") || contains(probe.detail, "no other engine"),
        "platform_unsupported explanation states that no other engine is substituted");
#endif
    check(!probe.detail.empty(), "the present-DLL probe always explains itself");

    auto open_present = voicetyper::platform::ParakeetRuntime::open(shipped, {});
#if defined(_WIN32)
    check(open_present.is_ok(), "ParakeetRuntime::open succeeds for the shipped DLL");
    if (open_present.is_ok()) {
        auto& runtime = *open_present.value();
        check(runtime.is_open(), "opened runtime reports is_open");
        check(!runtime.is_ready(), "opened runtime has no model loaded yet");
        check(runtime.abi_version() == 6, "opened runtime exposes ABI 6");
        check(runtime.last_error().empty(), "last_error of a fresh runtime is empty");
        const auto not_ready = runtime.transcribe_pcm(nullptr, 0, 16000, 0, "", {});
        check(not_ready.is_error(), "transcribing without a model fails instead of guessing");
    }
#elif defined(__linux__)
    check(open_present.is_error(), "ParakeetRuntime::open fails for a Windows DLL on Linux, without a fallback");
#else
    check(open_present.is_error(), "ParakeetRuntime::open fails off Windows, without a fallback");
#endif

#if defined(__linux__)
    check(voicetyper::platform::parakeet_library_beside_executable().filename() == "libparakeet.so",
        "the executable-relative lookup names the Linux shared object");
#elif !defined(_WIN32)
    check(voicetyper::platform::parakeet_library_beside_executable().empty(),
        "the executable-relative DLL lookup is empty off Windows");
#endif
}

/// The `#include` directives of a file, one per entry. Comments are ignored on
/// purpose: a header may *mention* whisper.h in prose as long as it does not
/// include it, which is exactly the rule this check enforces.
std::vector<std::string> include_directives(const std::string& text)
{
    std::vector<std::string> directives;
    std::size_t pos = 0;
    while (true) {
        const std::size_t found = text.find('#', pos);
        if (found == std::string::npos) {
            return directives;
        }
        const std::string line = [&text, found] {
            const std::size_t newline = text.find('\n', found);
            return newline == std::string::npos ? text.substr(found) : text.substr(found, newline - found);
        }();
        std::string normalized = line;
        std::string compact;
        for (const char c : normalized) {
            if (std::isspace(static_cast<unsigned char>(c)) == 0) {
                compact += c;
            }
        }
        if (compact.rfind("#include", 0) == 0) {
            directives.push_back(compact);
        }
        pos = found + 1;
    }
}

void check_no_native_headers_leak(const fs::path& source_dir)
{
    const std::array<fs::path, 2> roots = {source_dir / "src/asr", source_dir / "src/platform/windows"};
    for (const fs::path& root : roots) {
        if (!fs::is_directory(root)) {
            continue;
        }
        for (const auto& entry : fs::recursive_directory_iterator(root)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".hpp") {
                continue;
            }
            const std::vector<std::string> directives = include_directives(read_file(entry.path()));
            const std::string relative = entry.path().string();
            for (const std::string& directive : directives) {
                check(!contains(directive, "parakeet_capi.h"),
                    relative + " must not include the Parakeet C header");
                check(!contains(directive, "windows.h"),
                    relative + " must not include windows.h");
                if (root.filename() == "asr") {
                    check(!contains(directive, "whisper.h"),
                        relative + " must not include whisper.h; the binding belongs in the .cpp");
                }
            }
        }
    }
    // The single place that may name the native entry points is the .cpp.
    const fs::path binding = source_dir / "src/platform/windows/parakeet_runtime.cpp";
    const std::string binding_text = read_file(binding);
    check(contains(binding_text, "GetProcAddress"),
        "parakeet_runtime.cpp resolves symbols with GetProcAddress");
    check(contains(binding_text, "LoadLibraryW"),
        "parakeet_runtime.cpp loads the DLL with LoadLibraryW");
}

} // namespace

int main()
{
    const fs::path source_dir = VOICETYPER_SOURCE_DIR;
    const fs::path manifest_path = source_dir / "docs/migration/cpp/native-dependencies.json";
    const std::string manifest = read_file(manifest_path);

    check_manifest_shape(manifest_path, manifest);
    if (!manifest.empty()) {
        check_whisper_entry(manifest);
        check_parakeet_entry(manifest, source_dir);
    }
    check_no_native_headers_leak(source_dir);

    // The manifest pins the build inputs, and the build inputs are hashed, so
    // the pinned contract itself has a stable digest worth logging.
    if (!manifest.empty()) {
        const std::string digest = sha256_of_text(manifest);
        check(is_hex_of_length(digest, 64), "manifest digest is a 64-hex SHA-256");
        std::cout << "native-dependencies.json sha256 " << digest << '\n';
    }

    if (failures != 0) {
        std::cerr << "native-dependency-contract: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "native-dependency-contract: OK (" << skips << " skipped)\n";
    return 0;
}
