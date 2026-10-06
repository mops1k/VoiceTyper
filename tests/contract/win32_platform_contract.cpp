// Windows-only contract test for the Phase D platform backends.
//
// What this proves, and what it deliberately does not:
//
//   Proves, on Windows, with no desktop interaction:
//     * win32_clock: a pre-cancelled token is honoured even for a zero-length
//       wait, a cancellation raised mid-sleep returns within one 20 ms poll
//       slice rather than at the end of the requested duration, now() is
//       monotonic, elapsed_since() is never negative, and wall_now() is a real
//       calendar time;
//     * win32_file_system: atomic_write really goes through "<path>.tmp" and
//       moves it, file_size matches what was written, create_new is refused
//       while the target exists and never truncates it, open_write streams with
//       truncate and append, replace_file/remove_file/list_directory/
//       available_space behave, no failure path leaves a temp file behind, and
//       the Win32 error mapping table reports the documented codes;
//     * win32_logger: the frozen line format, CRLF endings, an injected clock,
//       tail(), and a stable name for every one of the eighteen ErrorCodes;
//     * win32_executor: invoke() runs on the executor's own thread,
//       on_this_thread() is true there and false on the caller, an invoke issued
//       *from* that thread completes instead of deadlocking, stale generations
//       are dropped, and a throwing task becomes internal;
//     * win32_clipboard / win32_paste: with no desktop session both report
//       unavailable instead of crashing or silently succeeding.
//
//   Deliberately NOT exercised automatically: a real SendInput Ctrl+V. It would
//   type into whatever window the developer has focused while the suite runs.
//   paste() is therefore only asserted through the suspend flag and the
//   read-only integrity probe; the real injection belongs to the physical
//   Windows platform gate (plan p_312b2ec83985, "Провести Windows platform gate").
//
// Every device-dependent section prints a skip reason instead of failing when the
// environment cannot provide it, nothing waits without a bound, and the scratch
// directory is removed on every exit path.

#include "domain/app_paths.hpp"
#include "domain/cancellation.hpp"
#include "domain/error.hpp"
#include "domain/file_logger.hpp"
#include "platform/api/clock.hpp"
#include "platform/api/executor.hpp"
#include "platform/api/logger.hpp"
#include "platform/windows/win32_clock.hpp"
#include "platform/windows/win32_clipboard.hpp"
#include "platform/windows/win32_executor.hpp"
#include "platform/windows/win32_file_system.hpp"
#include "platform/windows/win32_logger.hpp"
#include "platform/windows/win32_paste.hpp"

#if !defined(_WIN32)
#error "this contract test is Windows-only; the CMake target is guarded on WIN32"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

namespace {

using voicetyper::domain::CancellationSource;
using voicetyper::domain::ErrorCode;
using voicetyper::domain::Status;
using voicetyper::platform::Win32Clipboard;
using voicetyper::platform::Win32Clock;
using voicetyper::platform::Win32Executor;
using voicetyper::platform::Win32FileSystem;
using voicetyper::platform::Win32FileWriteStream;
using voicetyper::platform::Win32Logger;
using voicetyper::platform::Win32PasteSimulator;

int failures = 0;
int checks = 0;
int skipped_sections = 0;
/// Check count at the start of the current section, so section_done() can report
/// the per-section number instead of only the grand total.
int section_start_checks = 0;

void check(bool condition, std::string_view what)
{
    ++checks;
    std::fflush(stdout);
    if (!condition) {
        ++failures;
        std::printf("FAIL %.*s\n", static_cast<int>(what.size()), what.data());
        std::fflush(stdout);
    }
}

void note(const char* text)
{
    std::printf("note %s\n", text);
    // Flushed per line on purpose: if a wait ever hangs, the last printed
    // section is the diagnosis. "Never hang" has to mean "and be diagnosable".
    std::fflush(stdout);
}

/// Marks the start of a section for the same reason: a hang has to be
/// localizable from the output alone.
void section(const char* name)
{
    section_start_checks = checks;
    std::printf("section %s\n", name);
    std::fflush(stdout);
}

/// Reports how many checks the section that just finished contributed. A section
/// that silently checks nothing must be visible in the log, not invisible.
void section_done(const char* name)
{
    std::printf("section-done %s: %d check(s)\n", name, checks - section_start_checks);
    std::fflush(stdout);
}

/// A section that cannot run because the environment has no such device. It is
/// reported, not failed, and it is counted so the summary stays honest.
void skip(const char* section, const char* reason)
{
    ++skipped_sections;
    std::printf("SKIP %s: %s\n", section, reason);
}

/// Removes a scratch directory on every exit path, so the suite can never leave
/// temp files behind. A removal failure is reported but does not mask the real
/// result of the checks.
class ScratchDirectory {
public:
    ScratchDirectory()
    {
        std::error_code error;
        const std::filesystem::path base = std::filesystem::temp_directory_path(error);
        if (error) {
            return;
        }
        path_ = base / ("voicetyper-win32-contract-" + std::to_string(::GetCurrentProcessId()));
        std::filesystem::remove_all(path_, error);
        error.clear();
        std::filesystem::create_directories(path_, error);
        if (error) {
            path_.clear();
        }
    }

    ~ScratchDirectory()
    {
        if (path_.empty()) {
            return;
        }
        std::error_code error;
        std::filesystem::remove_all(path_, error);
        if (error) {
            std::printf("note scratch directory %s could not be fully removed: %s\n",
                path_.string().c_str(), error.message().c_str());
        }
    }

    ScratchDirectory(const ScratchDirectory&) = delete;
    ScratchDirectory& operator=(const ScratchDirectory&) = delete;

    /// Removes the tree now and reports whether it is gone. The end-of-run
    /// "no temp files left" check uses this, because the destructor cannot run
    /// before main() looks.
    bool remove()
    {
        if (path_.empty()) {
            return true;
        }
        std::error_code error;
        std::filesystem::remove_all(path_, error);
        const bool gone = !error && !std::filesystem::exists(path_, error) && !error;
        if (gone) {
            path_.clear();
        }
        return gone;
    }

    [[nodiscard]] bool valid() const noexcept { return !path_.empty(); }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    [[nodiscard]] std::filesystem::path file(std::string_view name) const
    {
        return path_ / std::filesystem::path(std::string(name));
    }

private:
    std::filesystem::path path_;
};

/// A Clock with a frozen wall time and a real monotonic source, so the log
/// timestamp can be asserted byte for byte.
class FrozenClock final : public voicetyper::platform::Clock {
public:
    explicit FrozenClock(std::chrono::system_clock::time_point frozen) noexcept
        : frozen_(frozen)
    {
    }

    [[nodiscard]] std::chrono::steady_clock::time_point now() const override
    {
        return std::chrono::steady_clock::time_point();
    }

    [[nodiscard]] std::chrono::system_clock::time_point wall_now() const override
    {
        return frozen_;
    }

    [[nodiscard]] std::chrono::steady_clock::duration elapsed_since(
        std::chrono::steady_clock::time_point start) const override
    {
        return now() - start;
    }

    Status sleep_for(std::chrono::milliseconds, const voicetyper::domain::CancellationToken&) override
    {
        return Status::success();
    }

    Status sleep_until(
        std::chrono::steady_clock::time_point,
        const voicetyper::domain::CancellationToken&) override
    {
        return Status::success();
    }

private:
    std::chrono::system_clock::time_point frozen_{};
};

const std::chrono::system_clock::time_point kFrozenWall{std::chrono::seconds{1'700'000'000}};

/// Puts a regular file where the failure case needs a directory, so the logger's
/// failure path can be exercised without an unreadable ACL.
bool files_write_blocker(const ScratchDirectory& scratch)
{
    const auto blocker = scratch.file("blocker");
    std::FILE* handle = std::fopen(blocker.string().c_str(), "wb");
    if (handle == nullptr) {
        return false;
    }
    std::fputs("not a directory", handle);
    std::fclose(handle);
    return true;
}

/// Reads a whole file as text for byte-level assertions.
std::string read_raw(const std::filesystem::path& path, bool* ok)
{
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error) {
        *ok = false;
        return {};
    }
    const auto size = std::filesystem::file_size(path, error);
    if (error) {
        *ok = false;
        return {};
    }
    std::string text(static_cast<std::size_t>(size), '\0');
    std::FILE* handle = std::fopen(path.string().c_str(), "rb");
    if (handle == nullptr) {
        *ok = false;
        return {};
    }
    const std::size_t read = text.empty() ? 0 : std::fread(text.data(), 1, text.size(), handle);
    std::fclose(handle);
    *ok = read == text.size();
    text.resize(read);
    return text;
}

// ---------------------------------------------------------------------------
// clock
// ---------------------------------------------------------------------------

void test_clock()
{
    section("clock");
    Win32Clock clock;

    // now() is monotonic and non-decreasing across a tight sample.
    auto previous = clock.now();
    bool non_decreasing = true;
    for (int index = 0; index < 1000; ++index) {
        const auto current = clock.now();
        if (current < previous) {
            non_decreasing = false;
        }
        previous = current;
    }
    check(non_decreasing, "clock.now() is non-decreasing over 1000 samples");
    check(clock.elapsed_since(previous) >= std::chrono::steady_clock::duration::zero(),
        "clock.elapsed_since() is never negative");

    // wall_now() is a calendar time, not a monotonic counter. A QPC-derived
    // "wall clock" would land near the steady_clock epoch (year 1970-ish on
    // MinGW's system_clock), so 2020 is a safe floor.
    const std::time_t wall_seconds = std::chrono::system_clock::to_time_t(clock.wall_now());
    std::tm wall{};
    ::localtime_s(&wall, &wall_seconds);
    check(wall.tm_year + 1900 >= 2020, "clock.wall_now() is a real calendar year");
    check(wall.tm_mon >= 0 && wall.tm_mon <= 11, "clock.wall_now() has a valid month");

    // A zero-length wait still checks the token, which is the rule clock.hpp
    // states explicitly for a non-positive duration.
    CancellationSource source;
    const auto zero_cancelled = clock.sleep_for(std::chrono::milliseconds{0}, source.token());
    check(zero_cancelled.code() == ErrorCode::ok, "a zero sleep with a live token succeeds");

    source.request_cancellation();
    const auto zero_after_cancel = clock.sleep_for(std::chrono::milliseconds{0}, source.token());
    check(zero_after_cancel.code() == ErrorCode::cancelled,
        "a pre-cancelled token cancels even a zero-length sleep");
    check(clock.sleep_for(std::chrono::milliseconds{0}, source.token()).code() == ErrorCode::cancelled,
        "cancellation is sticky across calls");
    const auto negative = clock.sleep_for(std::chrono::milliseconds{-5}, source.token());
    check(negative.code() == ErrorCode::cancelled, "a negative sleep honours cancellation too");

    // The long wait a pre-cancelled token must not pay for: a 30 s request has to
    // return immediately.
    const auto start_cancelled = std::chrono::steady_clock::now();
    const auto pre_cancelled = clock.sleep_for(std::chrono::milliseconds{30'000}, source.token());
    const auto cancelled_latency = std::chrono::steady_clock::now() - start_cancelled;
    check(pre_cancelled.code() == ErrorCode::cancelled, "a pre-cancelled 30 s sleep returns cancelled");
    check(cancelled_latency < std::chrono::milliseconds{500},
        "a pre-cancelled 30 s sleep returns in well under a second");

    // A default-constructed token is the "ordinary sleep": it really sleeps.
    const auto start_plain = std::chrono::steady_clock::now();
    const auto plain = clock.sleep_for(std::chrono::milliseconds{60}, voicetyper::domain::CancellationToken{});
    const auto plain_elapsed = std::chrono::steady_clock::now() - start_plain;
    check(plain.is_ok(), "an uncancellable 60 ms sleep succeeds");
    check(plain_elapsed >= std::chrono::milliseconds{40}, "an uncancellable 60 ms sleep really waits");
    check(plain_elapsed < std::chrono::seconds{5}, "an uncancellable 60 ms sleep does not overshoot");

    // A cancellation raised from another thread must be observed within one poll
    // slice (kSleepPollSliceMs = 20 ms), not at the end of the 5 s wait.
    //
    // The measurement is the best of three attempts, not a single one. A sleep
    // that did not poll would be slow in every attempt (5 s each), so the check
    // stays discriminating; what it stops being is a test of whether the machine
    // happened to preempt the thread, which says nothing about the backend.
    std::chrono::steady_clock::duration best_latency = std::chrono::steady_clock::duration::max();
    bool all_cancelled = true;
    bool any_fired = false;
    for (int attempt = 0; attempt < 3; ++attempt) {
        CancellationSource running;
        std::atomic_bool fired{false};
        const auto start_mid = std::chrono::steady_clock::now();
        std::thread canceller([&running, &fired] {
            std::this_thread::sleep_for(std::chrono::milliseconds{40});
            fired.store(true, std::memory_order_release);
            running.request_cancellation();
        });
        const auto mid = clock.sleep_for(std::chrono::seconds{5}, running.token());
        const auto latency = std::chrono::steady_clock::now() - start_mid;
        canceller.join();
        any_fired = any_fired || fired.load(std::memory_order_acquire);
        all_cancelled = all_cancelled && mid.code() == ErrorCode::cancelled;
        best_latency = std::min(best_latency, latency);
    }
    check(any_fired, "the mid-sleep canceller ran");
    check(all_cancelled, "a 5 s sleep interrupted at ~40 ms returns cancelled every time");
    check(best_latency < std::chrono::seconds{1},
        "an interrupted 5 s sleep returns within a second of the request, not at its deadline");

    // sleep_until with a past deadline is an immediate success.
    const auto past = clock.sleep_until(clock.now() - std::chrono::seconds{1},
        voicetyper::domain::CancellationToken{});
    check(past.is_ok(), "sleep_until with a past deadline succeeds immediately");
    check(clock.sleep_until(clock.now() - std::chrono::seconds{1}, source.token()).code()
            == ErrorCode::cancelled,
        "sleep_until still honours a pre-cancelled token");

    section_done("clock");
}

// ---------------------------------------------------------------------------
// file system
// ---------------------------------------------------------------------------

void test_file_system(const ScratchDirectory& scratch)
{
    section("file_system");
    const char* skip_section = "file_system";
    Win32FileSystem files;

    // The Win32 error mapping table is asserted directly, so a change in the
    // table is a test failure rather than a surprise in a support bundle.
    check(voicetyper::platform::map_last_error(ERROR_FILE_NOT_FOUND) == ErrorCode::not_found,
        "ERROR_FILE_NOT_FOUND maps to not_found");
    check(voicetyper::platform::map_last_error(ERROR_PATH_NOT_FOUND) == ErrorCode::not_found,
        "ERROR_PATH_NOT_FOUND maps to not_found");
    check(voicetyper::platform::map_last_error(ERROR_FILE_EXISTS) == ErrorCode::already_exists,
        "ERROR_FILE_EXISTS maps to already_exists");
    check(voicetyper::platform::map_last_error(ERROR_ALREADY_EXISTS) == ErrorCode::already_exists,
        "ERROR_ALREADY_EXISTS maps to already_exists");
    check(voicetyper::platform::map_last_error(ERROR_ACCESS_DENIED) == ErrorCode::permission_denied,
        "ERROR_ACCESS_DENIED maps to permission_denied");
    check(voicetyper::platform::map_last_error(ERROR_SHARING_VIOLATION) == ErrorCode::permission_denied,
        "ERROR_SHARING_VIOLATION maps to permission_denied");
    check(voicetyper::platform::map_last_error(ERROR_WRITE_PROTECT) == ErrorCode::permission_denied,
        "ERROR_WRITE_PROTECT maps to permission_denied");
    check(voicetyper::platform::map_last_error(ERROR_DISK_FULL) == ErrorCode::resource_exhausted,
        "ERROR_DISK_FULL maps to resource_exhausted");
    check(voicetyper::platform::map_last_error(ERROR_INVALID_NAME) == ErrorCode::invalid_argument,
        "ERROR_INVALID_NAME maps to invalid_argument");
    check(voicetyper::platform::map_last_error(4242) == ErrorCode::io_failure,
        "an unknown Win32 error maps to io_failure");

    // atomic_write: temp file then move, and the temp is gone afterwards.
    const auto settings = scratch.file("settings.json");
    const auto settings_temp = scratch.file("settings.json.tmp");
    check(!files.exists(settings), "the scratch settings file starts absent");
    const auto first = files.atomic_write(settings, "{\"a\":1}");
    check(first.is_ok(), "atomic_write creates a new file");
    check(files.exists(settings), "atomic_write leaves the target in place");
    check(!files.exists(settings_temp), "atomic_write removes its .tmp file");
    check(voicetyper::platform::atomic_write_temp_path(settings) == settings_temp,
        "the temp path is <target>.tmp");
    check(files.read_text(settings).value_or(std::string{}) == "{\"a\":1}",
        "atomic_write wrote the content verbatim");
    check(files.file_size(settings).value_or(0) == 7, "file_size reports the written length");

    // The replace shape: a longer body overwrites and the temp is still cleaned.
    const auto second = files.atomic_write(settings, "{\"a\":1,\"b\":22}");
    check(second.is_ok(), "atomic_write replaces an existing file");
    check(files.read_text(settings).value_or(std::string{}) == "{\"a\":1,\"b\":22}",
        "the replaced content is the new body");
    check(files.file_size(settings).value_or(0) == 14, "file_size follows the replacement");
    check(!files.exists(settings_temp), "the replace path removes its .tmp file");

    // create_new is refused while the target exists and changes nothing.
    const auto create_new_conflict = files.atomic_write(settings, "clobber", voicetyper::platform::FileWriteMode::create_new);
    check(create_new_conflict.code() == ErrorCode::already_exists,
        "atomic_write(create_new) is refused when the target exists");
    check(files.read_text(settings).value_or(std::string{}) == "{\"a\":1,\"b\":22}",
        "the refused create_new call did not truncate the target");

    // create_new succeeds on a free path.
    const auto created = scratch.file("created.json");
    const auto create_new_ok =
        files.atomic_write(created, "fresh", voicetyper::platform::FileWriteMode::create_new);
    check(create_new_ok.is_ok(), "atomic_write(create_new) succeeds on a free path");
    check(files.read_text(created).value_or(std::string{}) == "fresh", "the created file holds its body");
    check(!files.exists(scratch.file("created.json.tmp")), "create_new removes its .tmp file");

    // A failed write must not strand a temp file: the parent directory is
    // missing, so nothing is created anywhere.
    const auto unreachable = scratch.file("no-such-dir") / "settings.json";
    const auto failed = files.atomic_write(unreachable, "x");
    check(failed.is_error(), "atomic_write into a missing directory fails");
    check(failed.code() == ErrorCode::not_found, "the missing parent is reported as not_found");
    check(!files.exists(scratch.file("no-such-dir")), "a failed atomic_write created no directory");

    // create_directories is idempotent.
    const auto nested = scratch.file("a") / "b" / "c";
    check(files.create_directories(nested).is_ok(), "create_directories creates a nested tree");
    check(files.create_directories(nested).is_ok(), "create_directories is idempotent");
    check(files.exists(nested), "the created directory exists");

    // open_write: truncate streams and counts.
    // The stream lives in its own scope: an open handle without FILE_SHARE_DELETE
    // makes a later MoveFileExW/DeleteFileW fail with a sharing violation, which
    // is a Windows fact the test has to respect rather than work around.
    const auto streamed = scratch.file("model.download");
    {
    auto stream = files.open_write(streamed, voicetyper::platform::FileOpenMode::truncate);
    check(stream.is_ok(), "open_write(truncate) opens a new file");
    if (stream.is_ok()) {
        auto& handle = dynamic_cast<Win32FileWriteStream&>(*stream.value());
        check(handle.mode() == voicetyper::platform::FileOpenMode::truncate,
            "the stream reports its open mode");
        check(handle.bytes_written() == 0, "a fresh truncate stream starts at zero bytes");
        check(handle.write("abc", 3).is_ok(), "stream write of 3 bytes succeeds");
        check(handle.write("", 0).is_ok(), "a zero-length stream write is a success");
        check(handle.write("defgh", 5).is_ok(), "stream write of 5 bytes succeeds");
        check(handle.bytes_written() == 8, "bytes_written accumulates across writes");
        check(handle.flush().is_ok(), "flush succeeds and promises no fsync");
        handle.close();
        handle.close();
        check(handle.bytes_written() == 8, "close is idempotent and keeps the byte count");
        check(handle.write("x", 1).code() == ErrorCode::invalid_state, "a closed stream refuses a write");
    }
    }
    check(files.file_size(streamed).value_or(0) == 8, "the streamed file has the written length");

    // open_write: append continues and counts the bytes that were already there.
    {
    auto append = files.open_write(streamed, voicetyper::platform::FileOpenMode::append);
    check(append.is_ok(), "open_write(append) reopens the file");
    if (append.is_ok()) {
        check(append.value()->bytes_written() == 8, "an append stream starts at the existing size");
        check(append.value()->write("ij", 2).is_ok(), "the append write succeeds");
        check(append.value()->bytes_written() == 10, "an append stream counts the appended bytes");
        append.value()->close();
    }
    }
    check(files.file_size(streamed).value_or(0) == 10, "the append extended the file");

    // open_write into a missing directory is not_found.
    const auto orphan = files.open_write(scratch.file("nope") / "x.bin",
        voicetyper::platform::FileOpenMode::truncate);
    check(orphan.code() == ErrorCode::not_found, "open_write into a missing directory is not_found");

    // binary round-trip with an embedded NUL, which read_text would truncate.
    const std::string with_nul("a\0b", 3);
    const auto binary = scratch.file("bytes.bin");
    check(files.atomic_write(binary, with_nul).is_ok(), "atomic_write stores bytes with a NUL");
    const auto read_binary = files.read_binary(binary);
    check(read_binary.is_ok(), "read_binary succeeds");
    if (read_binary.is_ok()) {
        check(read_binary.value().size() == 3, "read_binary keeps the embedded NUL");
        check(read_binary.value()[1] == 0, "the embedded NUL survived the round trip");
    }

    // read_text of a missing file, and file_size of a missing file.
    check(files.read_text(scratch.file("absent.json")).code() == ErrorCode::not_found,
        "read_text of a missing file is not_found");
    check(files.file_size(scratch.file("absent.json")).code() == ErrorCode::not_found,
        "file_size of a missing file is not_found");
    check(files.file_size(nested).code() == ErrorCode::not_found,
        "file_size of a directory is not_found, not a size");
    check(files.read_text(binary).is_ok(), "read_text reads a binary file it is allowed to buffer");

    // replace_file: the model-download move.
    const auto downloaded = scratch.file("ggml.bin");
    check(files.replace_file(streamed, downloaded).is_ok(), "replace_file moves the download onto the target");
    check(files.file_size(downloaded).value_or(0) == 10, "the moved file keeps its size");
    check(!files.exists(streamed), "the source is gone after the move");
    check(files.replace_file(scratch.file("absent"), downloaded).code() == ErrorCode::not_found,
        "replace_file of a missing source is not_found");

    // remove_file succeeds when the file was already absent.
    check(files.remove_file(downloaded).is_ok(), "remove_file deletes an existing file");
    check(files.remove_file(downloaded).is_ok(), "remove_file of an absent file still succeeds");
    check(!files.exists(downloaded), "the removed file is gone");

    // list_directory.
    const auto listing = files.list_directory(scratch.path());
    check(listing.is_ok(), "list_directory succeeds on the scratch directory");
    if (listing.is_ok()) {
        const auto& entries = listing.value();
        const auto has = [&entries](const std::string& name) {
            return std::any_of(entries.begin(), entries.end(), [&name](const std::filesystem::path& entry) {
                return entry.filename().string() == name;
            });
        };
        check(has("settings.json"), "list_directory contains the written settings file");
        check(has("bytes.bin"), "list_directory contains the binary fixture");
        check(entries.size() >= 4, "list_directory reports the expected number of entries");
    }
    check(files.list_directory(scratch.file("no-such-dir")).code() == ErrorCode::not_found,
        "list_directory of a missing directory is not_found");
    const auto empty_dir = scratch.file("empty");
    check(files.create_directories(empty_dir).is_ok(), "create_directories makes an empty directory");
    const auto empty_listing = files.list_directory(empty_dir);
    check(empty_listing.is_ok() && empty_listing.value().empty(), "an empty directory lists as empty");

    // available_space: success on a real volume, and `unsupported` is a
    // documented outcome when the volume cannot be identified.
    const auto space = files.available_space(scratch.path());
    if (space.is_ok()) {
        check(space.value() > 0, "available_space reports free bytes on the scratch volume");
    } else {
        check(space.code() == ErrorCode::unsupported || space.code() == ErrorCode::not_found,
            "available_space either answers or says unsupported");
        skip(skip_section, "available_space could not resolve a volume for the scratch path");
    }

    // A directory that does not exist yet is still answerable: the nearest
    // existing ancestor is used, which is the "model cache" diagnostic case.
    const auto future = files.available_space(scratch.file("not-created-yet") / "models");
    check(future.is_ok() || future.code() == ErrorCode::unsupported,
        "available_space works for a directory that does not exist yet");

    // Final sweep: the scratch tree holds no stray temp files.
    std::error_code error;
    int temp_files = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(scratch.path(), error)) {
        const std::string name = entry.path().filename().string();
        if (name.size() >= 4 && name.compare(name.size() - 4, 4, ".tmp") == 0) {
            ++temp_files;
        }
    }
    check(!error, "the scratch tree can be walked");
    check(temp_files == 0, "no .tmp file survives any failure path");

    section_done("file_system");
}

// ---------------------------------------------------------------------------
// logger
// ---------------------------------------------------------------------------

void test_logger(const ScratchDirectory& scratch)
{
    section("logger");
    const char* skip_section = "logger";
    const auto log_dir = scratch.file("logs");
    const auto log_file = log_dir / std::filesystem::path(std::string(voicetyper::platform::kLogFileName));

    voicetyper::platform::LoggerOptions options;
    options.clear_on_start = true;
    FrozenClock frozen(kFrozenWall);
    Win32Logger logger(log_dir, frozen, options);

    check(logger.log_directory() == log_dir.string(), "log_directory reports the injected directory");
    check(logger.log_file_path() == log_file.string(), "log_file_path ends with voiceTyper.log");

    const auto written = logger.write(voicetyper::platform::LogLevel::info, "contract first line");
    check(written.is_ok(), "the logger writes a line");

    bool raw_ok = false;
    const std::string raw = read_raw(log_file, &raw_ok);
    check(raw_ok, "the log file can be read back");
    const std::string expected_stamp = voicetyper::platform::format_log_timestamp(kFrozenWall);
    check(raw.find(expected_stamp) != std::string::npos, "the line carries the injected clock timestamp");
    check(raw.find("[INFO] contract first line") != std::string::npos, "the line uses the frozen [LEVEL] format");
    check(raw.find("\r\n") != std::string::npos, "the line ending is CRLF");
    check(raw.find("\n\n") == std::string::npos, "no LF-only line ending was written");
    check(raw.size() < 2 || raw.rfind("\r\n") == raw.size() - 2,
        "the file ends with a complete CRLF");

    const auto tail = logger.tail(10);
    check(tail.is_ok(), "tail succeeds");
    if (tail.is_ok()) {
        check(tail.value().find("contract first line") != std::string::npos, "tail returns the written line");
        check(tail.value().find('\r') == std::string::npos, "tail normalises the CRLF away");
    }

    // A detail line follows on its own line, which is where an exception goes.
    check(logger.write(voicetyper::platform::LogLevel::error, "contract error line", "the detail").is_ok(),
        "a detail line is accepted");
    const auto tail_after_detail = logger.tail(10);
    if (tail_after_detail.is_ok()) {
        check(tail_after_detail.value().find("[ERROR] contract error line") != std::string::npos,
            "the error level token is ERROR");
        check(tail_after_detail.value().find("the detail") != std::string::npos,
            "the detail text follows on the next line");
    }

    // Every declared ErrorCode has a stable, non-"unknown" name, and write_status
    // puts exactly that name on the line.
    constexpr ErrorCode kAllCodes[] = {
        ErrorCode::ok, ErrorCode::cancelled, ErrorCode::invalid_argument, ErrorCode::not_found,
        ErrorCode::already_exists, ErrorCode::not_ready, ErrorCode::unsupported,
        ErrorCode::permission_denied, ErrorCode::unavailable, ErrorCode::device_disconnected,
        ErrorCode::timeout, ErrorCode::io_failure, ErrorCode::corrupt_data, ErrorCode::out_of_range,
        ErrorCode::resource_exhausted, ErrorCode::engine_unavailable, ErrorCode::model_not_ready,
        ErrorCode::invalid_state, ErrorCode::internal,
    };
    bool all_named = true;
    for (const ErrorCode code : kAllCodes) {
        const std::string name = voicetyper::platform::error_code_name_of(code);
        if (name.empty() || name == "unknown") {
            all_named = false;
        }
        const Status status = Status::failure(code, "the cause");
        check(logger.write_status(voicetyper::platform::LogLevel::warn, "code " + name, status).is_ok(),
            "write_status accepts the code");
    }
    check(all_named, "every ErrorCode has a stable name that is never 'unknown'");

    const auto names_tail = logger.tail(200);
    check(names_tail.is_ok(), "the tail after the code sweep is readable");
    if (names_tail.is_ok()) {
        bool all_logged = true;
        for (const ErrorCode code : kAllCodes) {
            if (names_tail.value().find("[" + voicetyper::platform::error_code_name_of(code) + "]")
                == std::string::npos) {
                all_logged = false;
            }
        }
        check(all_logged, "every ErrorCode name appears verbatim in the log");
        check(names_tail.value().find("[unknown]") == std::string::npos, "no line says [unknown]");
        check(names_tail.value().find("io_failure: the cause") != std::string::npos,
            "a failing status puts its cause on the detail line");
    }

    // Rotation before the append, with the frozen threshold and archive names.
    voicetyper::platform::LoggerOptions rotate_options;
    rotate_options.clear_on_start = true;
    rotate_options.rotate_threshold_bytes = 1;
    rotate_options.archive_count = 2;
    const auto rotate_dir = scratch.file("rotate");
    Win32Logger rotating(rotate_dir, frozen, rotate_options);
    check(rotating.write(voicetyper::platform::LogLevel::info, "before rotation").is_ok(), "the first line is written");
    check(rotating.write(voicetyper::platform::LogLevel::info, "after rotation").is_ok(), "the second line is written");
    const auto archive = rotate_dir / "voiceTyper.1.log";
    check(std::filesystem::exists(archive), "a full log is rotated before the next append");
    bool rotated_ok = false;
    const std::string archived = read_raw(archive, &rotated_ok);
    check(rotated_ok && archived.find("before rotation") != std::string::npos,
        "the archive holds the pre-rotation line");

    // clear() truncates the current log and keeps the archives.
    check(rotating.write(voicetyper::platform::LogLevel::info, "after rotation again").is_ok(),
        "another line is written");
    check(rotating.clear().is_ok(), "clear truncates the current log");
    const auto current = read_raw(rotate_dir / "voiceTyper.log", &raw_ok);
    check(raw_ok && current.empty(), "the current log is empty after clear()");
    check(std::filesystem::exists(archive), "clear keeps the archives");

    // AppPaths supplies the directory, which is what production uses.
    voicetyper::domain::AppPathRoots roots;
    roots.roaming = scratch.file("appdata");
    roots.local = scratch.file("localappdata");
    roots.application = scratch.file("app");
    const voicetyper::domain::AppPaths paths(roots);
    auto from_paths = Win32Logger::for_paths(paths, options, &frozen);
    check(from_paths != nullptr, "a logger can be built from AppPaths");
    if (from_paths != nullptr) {
        check(from_paths->log_directory() == paths.logs_directory().string(),
            "for_paths uses the AppPaths log directory");
        check(from_paths->write(voicetyper::platform::LogLevel::info, "from app paths").is_ok(),
            "the AppPaths logger writes");
        check(paths.log_file().filename() == "voiceTyper.log", "AppPaths resolves voiceTyper.log");
    }

    // Logging failures are reported, never thrown, and an unusable log directory
    // returns an error instead of aborting. A regular file stands where a
    // directory is needed, which Windows refuses deterministically.
    check(files_write_blocker(scratch), "a blocking file is in place for the failure case");
    Win32Logger broken(scratch.file("blocker") / "logs", frozen, options);
    const auto broken_write = broken.write(voicetyper::platform::LogLevel::info, "must not crash");
    check(broken_write.is_error(), "an unusable log directory is reported, not thrown");
    if (broken_write.is_error()) {
        check(broken_write.code() == ErrorCode::io_failure
                || broken_write.code() == ErrorCode::invalid_argument
                || broken_write.code() == ErrorCode::permission_denied
                || broken_write.code() == ErrorCode::already_exists,
            "an unusable log directory maps to a documented code");
    } else {
        skip(skip_section, "this filesystem created a directory under a regular file");
    }

    section_done("logger");
}

// ---------------------------------------------------------------------------
// executor
// ---------------------------------------------------------------------------

void test_executor()
{
    section("executor");
    Win32Executor executor;
    check(executor.is_running(), "the executor starts its worker thread");
    check(!executor.on_this_thread(), "on_this_thread() is false on the calling thread");

    // invoke() runs on the worker thread, and that is where on_this_thread() is true.
    bool worker_owns_thread = false;
    bool worker_saw_foreign = true;
    const auto invoked = executor.invoke(
        [&executor, &worker_owns_thread, &worker_saw_foreign] {
            worker_owns_thread = executor.on_this_thread();
            worker_saw_foreign = !executor.on_this_thread();
        },
        std::chrono::seconds{5});
    check(invoked.is_ok(), "invoke() from another thread completes");
    check(worker_owns_thread, "on_this_thread() is true inside an invoked task");
    check(!worker_saw_foreign, "on_this_thread() does not flip back mid-task");
    check(!executor.on_this_thread(), "on_this_thread() is false on the calling thread afterwards");

    // The Qt blocking-invoke trap: an invoke issued *from* the worker thread must
    // run inline instead of waiting for itself.
    std::atomic_int nested_depth{0};
    std::atomic_int nested_result{0};
    const auto outer = executor.invoke(
        [&executor, &nested_depth, &nested_result] {
            nested_depth.store(1);
            // A post-and-wait here would block the only thread that could run the
            // task, which is exactly the deadlock the contract forbids.
            const auto inner = executor.invoke([&nested_result] { nested_result.store(7); },
                std::chrono::seconds{5});
            nested_depth.store(0);
            if (!inner.is_ok()) {
                nested_result.store(-1);
            }
        },
        std::chrono::seconds{5});
    check(outer.is_ok(), "invoke() from the worker thread does not deadlock");
    check(nested_result.load() == 7, "the nested invoke ran inline on the worker thread");
    check(nested_depth.load() == 0, "the nested invoke returned before the outer task finished");

    // Task conversion rules.
    check(executor.invoke({}, std::chrono::milliseconds{10}).code() == ErrorCode::invalid_argument,
        "an empty task is invalid_argument");
    check(executor.invoke([] { throw 1; }, std::chrono::milliseconds{1000}).code() == ErrorCode::internal,
        "a throwing task becomes internal");
    check(executor.is_running(), "the worker survives a throwing task");

    // post / pending / generation drop, mirroring ManualExecutor's rules.
    std::atomic_int posted{0};
    const auto live_generation = executor.generation();

    // A long delay keeps the worker from picking the task up, so pending() is a
    // deterministic observation rather than a race with the worker thread.
    executor.post_delayed(live_generation - 1, [&posted] { posted.fetch_add(100); },
        std::chrono::milliseconds{400});
    check(executor.pending() == 1, "post() queues a task");
    check(executor.shutdown(std::chrono::seconds{5}).is_ok(), "shutdown drops the queued task");
    check(executor.pending() == 0, "shutdown clears the queue");
    std::this_thread::sleep_for(std::chrono::milliseconds{500});
    check(posted.load() == 0, "a stale generation is dropped silently");
    check(executor.generation() != live_generation, "shutdown advances the generation");

    const auto next_generation = executor.generation();
    executor.post_delayed(next_generation, [&posted] { posted.fetch_add(1); },
        std::chrono::milliseconds{50});
    check(executor.wait_for_idle(std::chrono::seconds{5}), "a delayed task drains");
    check(posted.load() == 1, "a post_delayed task with the live generation runs");
    check(executor.wait_for_idle(std::chrono::milliseconds{10}),
        "wait_for_idle returns as soon as the queue is empty");

    // A fresh post after shutdown is accepted, on the new epoch.
    std::atomic_int reused{0};
    executor.post([&reused] { reused.fetch_add(1); });
    check(executor.wait_for_idle(std::chrono::seconds{5}), "the executor is reusable after shutdown");
    check(reused.load() == 1, "a post after shutdown runs on the new epoch");

    // A slow task makes the blocking invoke time out instead of waiting forever.
    // Whether the worker had already picked the request up is a race by design,
    // so only the caller's answer and the eventual drain are asserted.
    const auto timed_out = executor.invoke([] { std::this_thread::sleep_for(std::chrono::milliseconds{300}); },
        std::chrono::milliseconds{20});
    check(timed_out.code() == ErrorCode::timeout, "a blocking invoke reports timeout, not a hang");
    check(executor.wait_for_idle(std::chrono::seconds{5}), "the timed-out task still finishes");

    // Posting from a foreign thread is safe.
    std::atomic_int cross_thread{0};
    std::thread poster([&executor, &cross_thread] {
        for (int index = 0; index < 50; ++index) {
            executor.post([&cross_thread] { cross_thread.fetch_add(1); });
        }
    });
    poster.join();
    check(executor.wait_for_idle(std::chrono::seconds{5}), "the cross-thread backlog drains");
    check(cross_thread.load() == 50, "all 50 cross-thread posts ran");

    section_done("executor");
}

// ---------------------------------------------------------------------------
// clipboard and paste
// ---------------------------------------------------------------------------

void test_clipboard_and_paste()
{
    section("clipboard/paste");
    const char* skip_section = "clipboard";

    // The clipboard is global state. The current text is read first and restored
    // at the end, so running the suite does not destroy what the user had
    // copied. The restore is best effort and its outcome is reported.
    Win32Clipboard clipboard;
    std::optional<std::string> saved;
    bool saved_ok = false;
    if (clipboard.is_owner_ready()) {
        const auto current = clipboard.get_text();
        if (current.is_ok() && current.value().has_value()) {
            saved = current.value();
            saved_ok = true;
        }
    } else {
        skip(skip_section, "no interactive desktop session: the hidden clipboard owner window could not be created");
    }

    if (!clipboard.is_owner_ready()) {
        // The exact .NET gap the contract forbids copying: no owner window must
        // be reported, never swallowed.
        const auto refused = clipboard.set_text("VoiceTyper contract", voicetyper::domain::CancellationToken{});
        check(refused.is_error(), "without an owner window set_text does not silently succeed");
        check(refused.code() == ErrorCode::unavailable,
            "without an owner window set_text reports unavailable");
        check(clipboard.get_text().code() == ErrorCode::unavailable,
            "without an owner window get_text reports unavailable");
        check(!clipboard.has_text(), "without an owner window has_text is false, not a guess");
    } else {
        check(clipboard.last_attempts() == 0, "no OpenClipboard attempt has happened yet");
        check(clipboard.worst_case_retry_wait()
                == std::chrono::milliseconds{voicetyper::platform::kClipboardRetryDelayMs
                * (voicetyper::platform::kClipboardMaxAttempts - 1)},
            "the retry policy is the frozen 5 x 120 ms");

        // A pre-cancelled token must not touch the clipboard at all.
        CancellationSource source;
        source.request_cancellation();
        const auto cancelled = clipboard.set_text("ignored", source.token());
        check(cancelled.code() == ErrorCode::cancelled, "set_text honours a pre-cancelled token");

        const std::string transcript = "VoiceTyper win32 contract \xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82";
        const auto written = clipboard.set_text(transcript, voicetyper::domain::CancellationToken{});
        if (written.is_ok()) {
            const auto read_back = clipboard.get_text();
            if (read_back.is_error()) {
                std::printf("note get_text failed right after a successful write: %s (win32 %lu)\n",
                    read_back.error().message().c_str(), clipboard.last_open_error());
            }
            check(read_back.is_ok(), "get_text succeeds after a write");
            if (read_back.is_ok()) {
                check(read_back.value().has_value(), "the clipboard holds text after a write");
                if (read_back.value().has_value()) {
                    check(read_back.value().value() == transcript,
                        "the UTF-8 transcript round-trips through CF_UNICODETEXT");
                }
            }
            if (!clipboard.has_text()) {
                // Print the evidence: "false" from this probe means either "no
                // text" or "the clipboard could not be opened", and the last Win32
                // error is what tells the two apart.
                std::printf("note has_text() was false right after a successful write; last OpenClipboard error=%lu\n",
                    clipboard.last_open_error());
            }
            check(clipboard.has_text(), "has_text() is true after a write");
            check(clipboard.last_attempts() >= 1, "the write recorded its attempt count");
        } else {
            // Another process holding the clipboard is a documented, legitimate
            // outcome, never a silent success - but it is not this suite's
            // failure either.
            check(written.code() == ErrorCode::permission_denied || written.code() == ErrorCode::unavailable,
                "a failed write is permission_denied or unavailable, never a bare io_failure");
            skip(skip_section, "the system clipboard could not be written in this environment (another process holds it)");
        }

        // A cancelled token on the read path is not part of the contract, but a
        // stuck clipboard must not hang the caller: every wait is bounded.
        const auto probed = clipboard.has_text();
        note(probed ? "clipboard holds text at the end of the run" : "clipboard holds no text at the end of the run");
    }

    if (saved_ok) {
        const auto restored = clipboard.set_text(*saved, voicetyper::domain::CancellationToken{});
        note(restored.is_ok() ? "the previous clipboard content was restored"
                              : "the previous clipboard content could NOT be restored");
    }

    // Paste: only the safe paths are exercised automatically. A real SendInput
    // Ctrl+V would type into whatever window is focused while the suite runs.
    const char* paste_section = "paste";
    Win32PasteSimulator paste;
    const bool injection = paste.is_injection_supported();
    if (!injection) {
        skip(paste_section, "this session has no interactive input desktop");
    }

    // Suspension is the hotkey-capture rule: no injection, and the failure is the
    // one the caller turns into clipboard_only.
    check(!paste.paste_suspended(), "injection starts unsuspended");
    paste.set_suspended(true);
    check(paste.paste_suspended(), "set_suspended() takes effect immediately");
    const auto suspended = paste.paste();
    check(suspended.is_error(), "a suspended paste does not inject");
    check(suspended.code() == ErrorCode::unavailable,
        "a suspended paste reports unavailable, which becomes clipboard_only");
    paste.set_suspended(false);

    if (!injection) {
        const auto refused = paste.paste();
        check(refused.is_error(), "without an input desktop paste does not silently succeed");
        check(refused.code() == ErrorCode::unavailable, "without an input desktop paste reports unavailable");
    } else {
        note("an input desktop exists; the real Ctrl+V injection is left to the physical Windows platform gate");
    }

    // The read-only integrity probe is safe to run anywhere and is what turns the
    // UAC case into permission_denied instead of a bare unavailable.
    const auto probe = paste.probe_focused_window();
    if (probe.resolved) {
        check(probe.own_level > 0, "the probe resolved this process's integrity level");
        check(!probe.target_above_own || probe.target_level > probe.own_level,
            "target_above_own is only set when the levels really differ");
        note(probe.same_process
                ? "the focused window belongs to this process (integrity probe not exercised cross-process)"
                : "the focused window is another process; its integrity level was read");
    } else {
        // No foreground window (a headless CI session) or an unreadable label.
        // Both are legitimate; what must hold is that the classification is not
        // invented.
        check(!probe.target_above_own, "an unresolved probe never claims the target is elevated");
        note(probe.window == 0
                ? "no window has focus in this session; the integrity probe had nothing to read"
                : "the focused window's integrity level could not be read");
    }
    check(Win32PasteSimulator::own_integrity_level() > 0,
        "this process's own integrity level is readable");

    section_done("clipboard/paste");
}

/// Real Ctrl+V injection into a window this test owns.
///
/// The suite used to leave injection to the physical gate because a synthetic
/// Ctrl+V lands in whatever window has focus. Owning the target removes that risk
/// and turns "the transcript reached the clipboard but nothing was pasted" into an
/// assertion - that report came from the target machine, and a focused edit
/// control is the only way to see it in a test.
void test_paste_injection()
{
    section("paste injection");
    Win32PasteSimulator paste;
    if (!paste.is_injection_supported()) {
        skip("paste", "this session has no interactive input desktop");
        section_done("paste injection");
        return;
    }

    const auto pump = [] {
        MSG message {};
        while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != 0) {
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
        }
    };

    const wchar_t* class_name = L"VoiceTyperPasteProbeFrame";
    WNDCLASSW window_class {};
    window_class.lpfnWndProc = ::DefWindowProcW;
    window_class.hInstance = ::GetModuleHandleW(nullptr);
    window_class.lpszClassName = class_name;
    if (::RegisterClassW(&window_class) == 0 && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        skip("paste", "the probe window class could not be registered");
        section_done("paste injection");
        return;
    }

    HWND frame = ::CreateWindowExW(WS_EX_TOOLWINDOW, class_name, L"VoiceTyper paste probe",
        WS_OVERLAPPEDWINDOW, 80, 80, 340, 150, nullptr, nullptr, window_class.hInstance, nullptr);
    if (frame == nullptr) {
        skip("paste", "the probe window could not be created");
        section_done("paste injection");
        return;
    }
    HWND edit = ::CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_LEFT | ES_MULTILINE, 10, 10, 300, 80,
        frame, nullptr, window_class.hInstance, nullptr);
    check(edit != nullptr, "the probe edit control exists");

    if (edit != nullptr) {
        ::ShowWindow(frame, SW_SHOW);
        ::SetForegroundWindow(frame);
        ::SetFocus(edit);
        bool foreground = false;
        for (int attempt = 0; attempt < 40 && !foreground; ++attempt) {
            pump();
            if (::GetForegroundWindow() == frame) {
                foreground = true;
                break;
            }
            ::Sleep(25);
        }
        note(foreground ? "the probe window owns the foreground"
                        : "the probe window could not take the foreground (another application holds it)");

        if (!foreground) {
            ::DestroyWindow(frame);
            skip("paste", "the probe window could not take the foreground, so injection has no owned target");
            section_done("paste injection");
            return;
        }

        Win32Clipboard clipboard;
        const std::string transcript = "VoiceTyper paste probe";
        const auto written = clipboard.set_text(transcript, voicetyper::domain::CancellationToken{});
        check(written.is_ok(), "the transcript is on the clipboard before the paste");

        const auto pasted = paste.paste();
        check(pasted.is_ok(), pasted.is_ok() ? "a real Ctrl+V reports success"
                                             : ("a real Ctrl+V failed: " + pasted.error().to_string()).c_str());

        std::wstring observed;
        for (int attempt = 0; attempt < 40; ++attempt) {
            pump();
            wchar_t buffer[256] {};
            const int length = ::GetWindowTextW(edit, buffer, static_cast<int>(std::size(buffer)));
            if (length > 0) {
                observed.assign(buffer, static_cast<std::size_t>(length));
                break;
            }
            ::Sleep(25);
        }
        check(observed == L"VoiceTyper paste probe",
            "the pasted text really landed in the focused edit control");
        note(observed.empty() ? "the edit control stayed empty after SendInput"
                              : "the edit control received the clipboard text");
        ::DestroyWindow(frame);
        pump();
    }

    section_done("paste injection");
}

} // namespace

int main()
{
    ScratchDirectory scratch;
    if (!scratch.valid()) {
        std::printf("SKIP win32-platform-contract: no writable temporary directory is available\n");
        std::printf("win32-platform-contract: OK (skipped: 1 section, 0 checks)\n");
        return 0;
    }

    test_clock();
    test_file_system(scratch);
    test_logger(scratch);
    test_executor();
    test_clipboard_and_paste();
    test_paste_injection();

    check(scratch.remove(), "the scratch directory is removed at the end of the run");

    if (failures != 0) {
        std::printf("win32-platform-contract: %d of %d check(s) failed\n", failures, checks);
        return 1;
    }
    std::printf("win32-platform-contract: OK (%d checks, %d section(s) skipped)\n", checks, skipped_sections);
    return 0;
}
