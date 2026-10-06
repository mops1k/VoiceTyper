// Update-core contract: the GitHub "latest release" manifest, the .NET version
// comparison and the installer hand-off script.
//
// Written BEFORE the implementation (ADR-012 / docs/tdd.md §1): this file is the
// specification of `src/core/support/update_manifest.hpp`,
// `src/core/support/update_launcher.hpp` and of the *already frozen*
// comparison helpers in `src/platform/api/updater.hpp`.
//
// What it pins, all of it without a network, a registry or a display server:
//   * the release JSON -> UpdateCheckResult mapping of
//     VoiceTyper.Core/Services/UpdateService.cs:63-143, including the asset pick
//     order, the failure messages and the fact that a failure is never an
//     exception;
//   * the .NET UpdateVersionComparer rules (UpdateService.cs:249-315) and the
//     tag/SHA-256 extraction helpers of the frozen contract, which this module
//     reuses instead of duplicating;
//   * the exact bytes of run-update.cmd (UpdateLauncher.cs:31-39) and the
//     cmd.exe arguments, so the launcher's script cannot drift byte-wise.
//
// It deliberately does NOT test the Win32 CreateProcessW call that starts the
// script: that lives in src/platform/windows/win32_update_launcher.cpp, which
// only compiles on Windows and is not part of this target.

#include "core/support/update_launcher.hpp"
#include "core/support/sha256.hpp"
#include "core/support/update_manifest.hpp"
#include "platform/api/updater.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace voicetyper;
using namespace voicetyper::core::support;

int failures = 0;

void check(bool condition, const std::string& message)
{
    if (!condition) {
        ++failures;
        std::cerr << "FAIL " << message << '\n';
    }
}

void check_equal(std::string_view actual, std::string_view expected, const std::string& message)
{
    if (actual != expected) {
        ++failures;
        std::cerr << "FAIL " << message << " (expected \"" << expected << "\", got \"" << actual << "\")\n";
    }
}

std::string read_binary(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
}

std::filesystem::path unique_directory()
{
    const auto stamp = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    return std::filesystem::temp_directory_path() / ("voicetyper-update-" + stamp);
}

// --- Fixtures ---------------------------------------------------------------
//
// Shaped like the real mops1k/VoiceTyper release: tag_name with a leading 'v',
// an installer asset plus extra assets in API order, a body carrying the
// "SHA256: <64 hex>" marker the release workflow appends.

constexpr std::string_view kNewerRelease = R"json({
  "tag_name": "v1.2.0",
  "name": "VoiceTyper 1.2.0",
  "prerelease": false,
  "body": "Fixes the overlay flicker.\n\nSHA256: 0123456789ABCDEF0123456789abcdef0123456789ABCDEF0123456789abcdef\n",
  "assets": [
    { "name": "source.tar.gz", "browser_download_url": "https://example.invalid/source.tar.gz", "size": 512 },
    { "name": "VoiceTyper-1.2.0-Setup.exe", "browser_download_url": "https://example.invalid/VoiceTyper-1.2.0-Setup.exe", "size": 45678901 },
    { "name": "VoiceTyper-1.2.0-Setup.exe.sha256", "browser_download_url": "https://example.invalid/VoiceTyper-1.2.0-Setup.exe.sha256", "size": 65 }
  ]
})json";

constexpr std::string_view kStableReleaseNoV = R"json({
  "tag_name": "1.2.0",
  "prerelease": false,
  "body": "Notes only.",
  "assets": [
    { "name": "VoiceTyper-1.2.0-Setup.exe", "browser_download_url": "https://example.invalid/VoiceTyper-1.2.0-Setup.exe", "size": 1 }
  ]
})json";

constexpr std::string_view kPrereleaseRelease = R"json({
  "tag_name": "v2.0.0-beta.1",
  "prerelease": true,
  "body": "Beta.",
  "assets": [
    { "name": "VoiceTyper-2.0.0-beta.1-Setup.exe", "browser_download_url": "https://example.invalid/VoiceTyper-2.0.0-beta.1-Setup.exe", "size": 7 }
  ]
})json";

// 1.1.10 > 1.1.9 numerically: a plain string or lexicographic compare would say
// the opposite, so this fixture pins the numeric-segment rule through the parser
// and not only through the comparer's own cases below.
constexpr std::string_view kNumericSegmentRelease = R"json({
  "tag_name": "v1.1.10",
  "prerelease": false,
  "body": "",
  "assets": [
    { "name": "VoiceTyper-1.1.10-Setup.exe", "browser_download_url": "https://example.invalid/VoiceTyper-1.1.10-Setup.exe", "size": 3 }
  ]
})json";

// Two assets both match the Setup regex: the FIRST one in API order wins, which
// is the .NET "first matching asset" rule and not "newest-looking name".
constexpr std::string_view kTwoSetupAssets = R"json({
  "tag_name": "v1.3.0",
  "prerelease": false,
  "body": "",
  "assets": [
    { "name": "VoiceTyper-1.3.0-Setup.exe", "browser_download_url": "https://example.invalid/first.exe", "size": 10 },
    { "name": "VoiceTyper-9.9.9-Setup.exe", "browser_download_url": "https://example.invalid/second.exe", "size": 20 }
  ]
})json";

constexpr std::string_view kNoSetupAsset = R"json({
  "tag_name": "v1.2.0",
  "prerelease": false,
  "body": "SHA256: 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
  "assets": [
    { "name": "VoiceTyper-Setup.exe", "browser_download_url": "https://example.invalid/x.exe", "size": 1 },
    { "name": "VoiceTyper-1.2.0-Setup.msi", "browser_download_url": "https://example.invalid/y.msi", "size": 1 },
    { "name": "", "browser_download_url": "https://example.invalid/z", "size": 1 }
  ]
})json";

constexpr std::string_view kSetupAssetWithoutUrl = R"json({
  "tag_name": "v1.2.0",
  "prerelease": false,
  "assets": [
    { "name": "VoiceTyper-1.2.0-Setup.exe", "size": 42 }
  ]
})json";

constexpr std::string_view kBlankUrlRelease = R"json({
  "tag_name": "v1.2.0",
  "prerelease": false,
  "assets": [
    { "name": "VoiceTyper-1.2.0-Setup.exe", "browser_download_url": "   ", "size": 42 }
  ]
})json";

// A non-string field is a protocol change. The .NET service throws out of
// GetString()/GetInt64() here; the port must report a failure instead, because
// "no exception crosses a module boundary" is a migration rule.
constexpr std::string_view kNonStringTag = R"json({
  "tag_name": 120,
  "prerelease": false,
  "assets": [
    { "name": "VoiceTyper-1.2.0-Setup.exe", "browser_download_url": "https://example.invalid/x.exe", "size": 1 }
  ]
})json";

constexpr std::string_view kNoPrereleaseField = R"json({
  "tag_name": "v1.2.0",
  "assets": [
    { "name": "VoiceTyper-1.2.0-Setup.exe", "browser_download_url": "https://example.invalid/x.exe" }
  ]
})json";

constexpr std::string_view kExampleUrl = "https://example.invalid/VoiceTyper-1.2.0-Setup.exe";

// --- Release parsing --------------------------------------------------------

void check_newer_release_is_available()
{
    const platform::UpdateCheckResult result = parse_latest_release(kNewerRelease, "1.1.3");
    check(result.is_available(), "a newer tag with a Setup asset reports update_available");
    if (!result.update.has_value()) {
        failures++;
        std::cerr << "FAIL update_available carries an UpdateInfo\n";
        return;
    }

    const platform::UpdateInfo& info = *result.update;
    check_equal(info.version, "1.2.0", "the leading 'v' is stripped from tag_name");
    check(info.installer_url.has_value() && *info.installer_url == kExampleUrl,
        "the installer URL comes from the matching asset");
    check(info.size_bytes.has_value() && *info.size_bytes == 45678901ULL, "the asset size is carried through");
    check(info.release_notes.has_value() && info.release_notes->find("overlay flicker") != std::string::npos,
        "the release body becomes release_notes");
    check(info.sha256.has_value() && *info.sha256
            == "0123456789ABCDEF0123456789abcdef0123456789ABCDEF0123456789abcdef",
        "the SHA-256 marker is extracted from the body");
    check(!info.is_prerelease, "prerelease false stays false");
    check(result.error.empty(), "an available update carries no error");
}

void check_first_matching_asset_wins()
{
    const platform::UpdateCheckResult result = parse_latest_release(kTwoSetupAssets, "1.0.0");
    check(result.is_available(), "two matching assets still report an update");
    check(result.update.has_value() && result.update->installer_url.has_value()
            && *result.update->installer_url == "https://example.invalid/first.exe",
        "the first matching asset in API order wins");
    check(result.update.has_value() && result.update->size_bytes.has_value()
            && *result.update->size_bytes == 10ULL,
        "the size belongs to the picked asset");
}

void check_same_or_older_version_is_up_to_date()
{
    check(parse_latest_release(kNewerRelease, "1.2.0").is_up_to_date(), "an equal version is up to date");
    check(parse_latest_release(kNewerRelease, "1.2.1").is_up_to_date(), "an older remote version is up to date");
    check(parse_latest_release(kNewerRelease, "2.0.0").is_up_to_date(), "a much older remote version is up to date");
    // 1.1.10 > 1.1.9 numerically, so this one must be available, not "up to date".
    const platform::UpdateCheckResult numeric = parse_latest_release(kNumericSegmentRelease, "1.1.9");
    check(numeric.is_available(), "1.1.10 is newer than the installed 1.1.9");
    check(numeric.update.has_value() && numeric.update->version == "1.1.10", "the 1.1.10 tag is kept verbatim");
    check(parse_latest_release(kNumericSegmentRelease, "1.1.10").is_up_to_date(),
        "1.1.10 against the installed 1.1.10 is up to date");
}

void check_tag_without_leading_v()
{
    const platform::UpdateCheckResult result = parse_latest_release(kStableReleaseNoV, "1.1.0");
    check(result.is_available(), "a tag without a leading 'v' is still a version");
    check(result.update.has_value() && result.update->version == "1.2.0",
        "a tag without 'v' is used as-is");
}

void check_prerelease_is_stored_not_filtered()
{
    const platform::UpdateCheckResult result = parse_latest_release(kPrereleaseRelease, "1.9.0");
    check(result.is_available(), "a prerelease is not filtered out");
    check(result.update.has_value() && result.update->is_prerelease, "the prerelease flag is stored");
    check(result.update.has_value() && result.update->version == "2.0.0-beta.1", "the prerelease tag is kept verbatim");
}

void check_missing_asset_fails_with_a_message()
{
    const platform::UpdateCheckResult result = parse_latest_release(kNoSetupAsset, "1.0.0");
    check(result.is_failed(), "a release without a Setup asset fails");
    check(!result.update.has_value(), "a failed result carries no update");
    check_equal(result.error, kUpdateMessageInstallerMissing, "the missing-asset message is the contract text");
}

void check_missing_url_fails_with_a_message()
{
    const platform::UpdateCheckResult missing = parse_latest_release(kSetupAssetWithoutUrl, "1.0.0");
    check(missing.is_failed(), "a Setup asset without a download URL fails");
    check_equal(missing.error, kUpdateMessageInstallerUrlMissing, "the missing-URL message is the contract text");

    const platform::UpdateCheckResult blank = parse_latest_release(kBlankUrlRelease, "1.0.0");
    check(blank.is_failed(), "a whitespace-only download URL fails");
    check_equal(blank.error, kUpdateMessageInstallerUrlMissing, "a blank URL uses the missing-URL message");
}

void check_malformed_and_blank_input_fail()
{
    const std::vector<std::string_view> broken{
        "",
        "   \r\n\t ",
        "{",
        "{\"tag_name\": }",
        "not json at all",
        "[1,2,3]",
        "\"a string\"",
    };
    for (const std::string_view json : broken) {
        const platform::UpdateCheckResult result = parse_latest_release(json, "1.0.0");
        check(result.is_failed(), "malformed or blank input fails: '" + std::string(json) + "'");
        check(!result.error.empty(), "a failed parse carries a message: '" + std::string(json) + "'");
        if (result.is_failed()) {
            check_equal(result.error, kUpdateMessageMalformedResponse,
                "broken JSON uses the malformed-response message");
        }
    }
}

void check_non_string_tag_is_not_an_exception()
{
    // tag_name is a number: the .NET code throws here (UpdateService.cs:84). The
    // port must degrade to "no version" and still fail on nothing else, i.e. an
    // empty version compares as older -> up to date.
    const platform::UpdateCheckResult result = parse_latest_release(kNonStringTag, "1.0.0");
    check(!result.is_failed(), "a non-string tag_name is not a failure of its own");
    check(result.is_up_to_date(), "a non-string tag_name degrades to an empty (older) version");
}

void check_missing_optional_fields()
{
    const platform::UpdateCheckResult result = parse_latest_release(kNoPrereleaseField, "1.0.0");
    check(result.is_available(), "a release without prerelease/body/size is still usable");
    check(result.update.has_value() && !result.update->is_prerelease, "an absent prerelease flag defaults to false");
    check(result.update.has_value() && !result.update->release_notes.has_value(), "an absent body yields no notes");
    check(result.update.has_value() && !result.update->size_bytes.has_value(), "an absent size yields no size");
    check(result.update.has_value() && !result.update->sha256.has_value(), "an absent SHA marker yields no hash");
}

void check_http_status_mapping()
{
    const auto not_found = failed_for_http_status(404);
    check(not_found.has_value() && not_found->is_failed(), "404 maps to a failure");
    if (not_found.has_value()) {
        check_equal(not_found->error, kUpdateMessageReleaseNotFound, "404 message");
    }

    const auto forbidden = failed_for_http_status(403);
    check(forbidden.has_value() && forbidden->is_failed(), "403 maps to a failure");
    if (forbidden.has_value()) {
        check_equal(forbidden->error, kUpdateMessageAccessDenied, "403 message");
    }

    const auto limited = failed_for_http_status(429);
    check(limited.has_value() && limited->is_failed(), "429 maps to a failure");
    if (limited.has_value()) {
        check_equal(limited->error, kUpdateMessageRateLimited, "429 message");
    }

    const auto other = failed_for_http_status(500);
    check(other.has_value() && other->is_failed() && other->error.find("500") != std::string::npos,
        "an unexpected status keeps the status code in the message");

    check(!failed_for_http_status(200).has_value(), "200 is not a failure: the body must be parsed");
    check(!failed_for_http_status(204).has_value(), "204 is not a failure: the body must be parsed");
}

// --- Frozen endpoint and comparison rules -----------------------------------

void check_frozen_endpoint_constants()
{
    check_equal(platform::kUpdateRepository, "mops1k/VoiceTyper", "the release repository is frozen");
    check_equal(platform::kUpdateApiBaseUrl, "https://api.github.com/repos/mops1k/VoiceTyper",
        "the release API base URL is frozen");
    check_equal(kSetupAssetRegex, "^VoiceTyper-\\d[^/]*?-Setup\\.exe$", "the Setup asset regex is frozen");
}

void check_setup_asset_names()
{
    const std::vector<std::pair<std::string_view, bool>> cases{
        {"VoiceTyper-1.1.3-Setup.exe", true},
        {"VoiceTyper-1-Setup.exe", true},
        {"VoiceTyper-2.0.0-beta.1-Setup.exe", true},
        {"VoiceTyper-2026.01-Setup.exe", true},
        {"VoiceTyper-Setup.exe", false},
        {"VoiceTyper-x1-Setup.exe", false},
        {"VoiceTyper-1.1.3-Setup.msi", false},
        {"VoiceTyper-1.1.3-Setup.exe.sha256", false},
        {"something-VoiceTyper-1.1.3-Setup.exe", false},
        {"VoiceTyper-1/1.3-Setup.exe", false},
        {"", false},
    };
    for (const auto& [name, expected] : cases) {
        check(is_setup_asset_name(name) == expected,
            "setup asset name '" + std::string(name) + "' is " + (expected ? "accepted" : "rejected"));
    }
}

void check_version_comparison()
{
    using platform::compare_update_versions;
    const std::vector<std::tuple<std::string_view, std::string_view, int>> cases{
        {"1.1.10", "1.1.9", 1},
        {"2.0.0", "1.9.9", 1},
        {"1.1.3", "1.1.3", 0},
        {"1.2.0-beta.1", "1.2.0", -1},
        {"1.2.0", "1.2.0-beta.1", 1},
        {"1.2.0-beta.1", "1.2.0-beta.2", 0},
        {"1.1.3", "1.1", 1},
        {"1.1.3+build.7", "1.1.3", 0},
        {"1.0.0", "1.0.0.0", 0},
        {"abc", "xyz", 0},
        {"", "1.0.0", -1},
        {"1.0.0", "", 1},
        {"", "", 0},
        // Invalid (overflow) segments become zero, never an error.
        {"99999999999999999999", "1.0.0", -1},
        // Whitespace is not trimmed.
        {" 1.1.3", "1.1.3", -1},
        {"1.1.3 ", "1.1.3", -1},
        // A leading 'v' is NOT the comparer's business: the parser strips it.
        {"v1.1.3", "1.1.3", -1},
    };
    for (const auto& [left, right, expected] : cases) {
        const int actual = compare_update_versions(left, right);
        const int sign = actual > 0 ? 1 : (actual < 0 ? -1 : 0);
        check(sign == expected,
            "compare(\"" + std::string(left) + "\", \"" + std::string(right) + "\") sign");
    }

    const std::vector<std::pair<std::string_view, std::string_view>> tags{
        {"v1.1.3", "1.1.3"},
        {"1.1.3", "1.1.3"},
        {"vv1.1.3", "1.1.3"},
        {"V1.1.3", "V1.1.3"},
        {"v", ""},
        {"", ""},
    };
    for (const auto& [tag, expected] : tags) {
        check_equal(platform::strip_version_tag_prefix(tag), expected, "TrimStart('v') on '" + std::string(tag) + "'");
    }
}

void check_sha256_extraction()
{
    const std::string digest(64, 'a');
    const auto marked = platform::extract_release_sha256("Release notes.\nSHA256: " + digest + "\n");
    check(marked.has_value() && *marked == digest, "SHA256: marker on its own line is extracted");

    const auto dashed = platform::extract_release_sha256("sha-256:" + std::string(64, 'B'));
    check(dashed.has_value() && *dashed == std::string(64, 'B'), "SHA-256: marker without spaces is extracted");

    check(!platform::extract_release_sha256("").has_value(), "no body means no hash");
    check(!platform::extract_release_sha256("SHA256: " + std::string(63, 'a')).has_value(), "a 63-hex line is not a hash");
    check(!platform::extract_release_sha256("SHA256: " + std::string(63, 'a') + "zzz").has_value(),
        "a non-hex line is not a hash");
    check(!platform::extract_release_sha256("See SHA256: " + digest).has_value(),
        "a marker not at the start of a line is ignored");
}

// --- Launcher script --------------------------------------------------------

void check_runner_script_bytes()
{
    constexpr std::string_view installer = R"(C:\Users\Александр\AppData\Local\VoiceTyper\updates\VoiceTyper-1.2.0-Setup.exe)";
    constexpr std::string_view app = R"(C:\Users\Александр\AppData\Local\Programs\VoiceTyper\VoiceTyper.exe)";
    const std::string script = build_update_runner_script(installer, app);

    const std::string expected = std::string("@echo off\r\n")
        + "start \"\" /wait \"" + std::string(installer) + "\" /AutoUpdate\r\n"
        + "start \"\" \"" + std::string(app) + "\"\r\n";
    check_equal(script, expected, "run-update.cmd is byte-identical to UpdateLauncher.cs");

    // Every LF is preceded by CR: the script is written with explicit CRLF and
    // must never gain a second CR on a text-mode Windows stream.
    bool crlf_only = true;
    for (std::size_t index = 0; index < script.size(); ++index) {
        if (script[index] == '\n' && (index == 0 || script[index - 1] != '\r')) {
            crlf_only = false;
        }
    }
    check(crlf_only, "the runner script uses CRLF line endings only");

    const std::size_t installer_line = script.find("start \"\" /wait \"");
    const std::size_t app_line = script.find("start \"\" \"" + std::string(app) + "\"");
    check(installer_line != std::string::npos && app_line != std::string::npos && installer_line < app_line,
        "the installer line comes before the app relaunch line");
    check(script.find("/AutoUpdate") != std::string::npos, "the installer is started with /AutoUpdate");
    check(script.find("/wait") != std::string::npos, "the installer is waited for");
    check(script.rfind("@echo off\r\n", 0) == 0, "the script starts with @echo off");

    // Empty paths still produce a well-formed, deterministic script: the
    // validation lives in the writer/launcher, not in the byte builder.
    check_equal(build_update_runner_script("", ""),
        "@echo off\r\nstart \"\" /wait \"\" /AutoUpdate\r\nstart \"\" \"\"\r\n",
        "empty paths keep the script shape");
}

void check_runner_arguments()
{
    check_equal(kUpdateRunnerProgram, "cmd.exe", "the runner is started through cmd.exe");
    check_equal(build_update_runner_arguments(R"(C:\Users\A\AppData\Local\VoiceTyper\updates\run-update.cmd)"),
        "/d /c \"C:\\Users\\A\\AppData\\Local\\VoiceTyper\\updates\\run-update.cmd\"",
        "cmd.exe arguments quote the runner path");
}

void check_runner_script_written_to_disk()
{
    const std::filesystem::path directory = unique_directory();
    std::error_code error;
    std::filesystem::create_directories(directory / "updates", error);
    if (error) {
        failures++;
        std::cerr << "FAIL could not create " << directory << ": " << error.message() << '\n';
        return;
    }

    const std::filesystem::path script_path = directory / "updates" / "run-update.cmd";
    const std::string installer = R"(C:\tmp\VoiceTyper-1.2.0-Setup.exe)";
    const std::string app = R"(C:\tmp\VoiceTyper.exe)";

    const domain::Status status = write_update_runner_script(script_path, installer, app);
    check(status.is_ok(), "writing the runner script succeeds");
    const std::string on_disk = read_binary(script_path);
    check_equal(on_disk, build_update_runner_script(installer, app), "the file on disk holds the exact script bytes");
    check(on_disk.find("\r\r\n") == std::string::npos, "the write did not double the CR of a CRLF pair");

    const domain::Status missing_parent
        = write_update_runner_script(directory / "absent" / "run-update.cmd", installer, app);
    check(missing_parent.is_error(), "writing into a missing directory is reported, not silently dropped");

    std::filesystem::remove_all(directory, error);
}

} // namespace

// The SHA-256 used to check a downloaded installer. The vectors are the standard ones
// (FIPS 180-4 / RFC 6234), plus a chunked update that must equal the one-shot hash.
static void sha256_vectors()
{
    using voicetyper::core::support::sha256_hex;
    using voicetyper::core::support::Sha256;

    check_equal(sha256_hex(std::string_view()),
        std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"),
        std::string("sha256 of the empty input"));
    check_equal(sha256_hex(std::string_view("abc")),
        std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),
        std::string("sha256 of \"abc\""));
    check_equal(sha256_hex(std::string_view("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")),
        std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"),
        std::string("sha256 of the 56-byte vector"));

    // A chunked hash must equal the one-shot one: the installer is hashed while it is
    // written, and the file is one hash, not one per chunk.
    Sha256 chunked;
    const std::string text = "VoiceTyper";
    for (const char ch : text) {
        const std::uint8_t byte = static_cast<std::uint8_t>(ch);
        chunked.update(std::span<const std::uint8_t>(&byte, 1));
    }
    check_equal(chunked.finish_hex(), sha256_hex(std::string_view(text)),
        std::string("a chunked hash equals the one-shot hash"));
}

int main()
{
    sha256_vectors();
    check_frozen_endpoint_constants();
    check_newer_release_is_available();
    check_first_matching_asset_wins();
    check_same_or_older_version_is_up_to_date();
    check_tag_without_leading_v();
    check_prerelease_is_stored_not_filtered();
    check_missing_asset_fails_with_a_message();
    check_missing_url_fails_with_a_message();
    check_malformed_and_blank_input_fail();
    check_non_string_tag_is_not_an_exception();
    check_missing_optional_fields();
    check_http_status_mapping();
    check_setup_asset_names();
    check_version_comparison();
    check_sha256_extraction();
    check_runner_script_bytes();
    check_runner_arguments();
    check_runner_script_written_to_disk();

    if (failures != 0) {
        std::cerr << "update-contract: " << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "update-contract: OK\n";
    return 0;
}
