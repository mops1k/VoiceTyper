#pragma once

// Filesystem contract.
//
// Evidence: docs/migration/cpp/compatibility-contracts.md §1 "Load/save
// behavior", §4 (".download" temp files) and §10 (failure-injection fixtures).
//
// Frozen observable contract:
//   * The settings save writes "settings.json.tmp" first and then replaces
//     "settings.json". The settings reader must therefore tolerate a leftover
//     temp file from a crashed write.
//   * A model or installer download writes "<target>.download" and is moved onto
//     the target when finished.
//   * The .NET implementation performs no fsync, no transactional replace and no
//     guaranteed temp cleanup. This contract does not silently strengthen that:
//     atomic_write() guarantees the temp-then-rename shape and nothing more.
//     Turning on durability (fsync) is a deliberate stability decision that needs
//     recovery and compatibility tests first, per compatibility-contracts.md §1.
//
// Load behavior of the settings layer (missing file -> defaults; unreadable,
// corrupt or unauthorized -> complete defaults) lives in the settings service,
// not here: this interface only reports what the filesystem did.
//
// Thread affinity: safe from any thread. The backend serializes any per-path
// bookkeeping it needs.
// Ownership: paths are values; no handle outlives a call.

#include "domain/cancellation.hpp"
#include "domain/error.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace voicetyper::platform {

using domain::CancellationToken;
using domain::ErrorCode;
using domain::Result;
using domain::Status;

/// Write mode for atomic_write().
enum class FileWriteMode : std::uint8_t {
    /// Replace the target, discarding whatever it contained. The settings and
    /// installer flows use this.
    replace = 0,
    /// Create a new file, failing with already_exists when one is there.
    create_new = 1,
};

/// How a streaming writer opens its target file.
enum class FileOpenMode : std::uint8_t {
    /// Create or truncate. The model download flow uses this for
    /// "<target>.download": a stale temp is never appended to, because appending
    /// garbage is how a "successful" download ends up corrupt.
    truncate = 0,
    /// Create or continue writing. Reserved for a future resume step; the model
    /// store does not use it while resume is disabled.
    append = 1,
};

/// A streaming binary writer. Model weights are up to ~940 MB, so the download
/// path cannot go through atomic_write()'s string_view payload.
///
/// Durability note: flush() pushes the OS buffer only. Nothing here promises
/// fsync, matching the existing atomic_write() contract and the .NET reference,
/// which performs no fsync at all. Callers still get the temp-then-rename
/// shape, which is what protects an interrupted download.
class FileWriteStream {
public:
    virtual ~FileWriteStream() = default;

    FileWriteStream(const FileWriteStream&) = delete;
    FileWriteStream& operator=(const FileWriteStream&) = delete;
    FileWriteStream(FileWriteStream&&) = delete;
    FileWriteStream& operator=(FileWriteStream&&) = delete;

    /// Appends `size` bytes. Failure: permission_denied, io_failure,
    /// resource_exhausted.
    virtual Status write(const char* data, std::size_t size) = 0;
    /// Total bytes written through this stream so far.
    [[nodiscard]] virtual std::uint64_t bytes_written() const noexcept = 0;
    /// Pushes the OS buffer out. Not fsync, see the durability note.
    virtual Status flush() = 0;
    /// Closes the handle. Idempotent; the destructor closes as well.
    virtual void close() noexcept = 0;

protected:
    FileWriteStream() = default;
};

class FileSystem {
public:
    virtual ~FileSystem() = default;

    FileSystem(const FileSystem&) = delete;
    FileSystem& operator=(const FileSystem&) = delete;
    FileSystem(FileSystem&&) = delete;
    FileSystem& operator=(FileSystem&&) = delete;

    /// Whether a file or directory exists at `path`.
    [[nodiscard]] virtual bool exists(const std::filesystem::path& path) const = 0;

    /// Size of a regular file. Failure: not_found, io_failure.
    [[nodiscard]] virtual Result<std::uint64_t> file_size(const std::filesystem::path& path) const = 0;

    /// Creates `path` and any missing parents. Succeeds when it already exists.
    /// Failure: permission_denied, io_failure.
    virtual Status create_directories(const std::filesystem::path& path) = 0;

    /// Opens a streaming writer. The caller owns the stream and must close it.
    /// Failure: permission_denied, io_failure.
    [[nodiscard]] virtual Result<std::unique_ptr<FileWriteStream>> open_write(
        const std::filesystem::path& path, FileOpenMode mode) = 0;

    /// Reads a whole text file. Failure: not_found, permission_denied,
    /// io_failure, and corrupt_data for a size the backend refuses to buffer.
    [[nodiscard]] virtual Result<std::string> read_text(const std::filesystem::path& path) const = 0;

    /// Reads a whole binary file. Used for model probes and WAV fixtures.
    [[nodiscard]] virtual Result<std::vector<std::uint8_t>> read_binary(
        const std::filesystem::path& path) const = 0;

    /// Writes `content` to "<path>.tmp" and then moves it onto `path`.
    ///
    /// This is the frozen settings/installer save shape. It guarantees the
    /// temp-then-rename sequence and that a failed write leaves `path`
    /// untouched; it does not promise durability.
    /// Failure: permission_denied, io_failure, resource_exhausted.
    virtual Status atomic_write(
        const std::filesystem::path& path, std::string_view content, FileWriteMode mode = FileWriteMode::replace) = 0;

    /// Moves `from` onto `to`, replacing `to`.
    /// Failure: not_found, permission_denied, io_failure.
    virtual Status replace_file(const std::filesystem::path& from, const std::filesystem::path& to) = 0;

    /// Deletes a file. Succeeds when it was already absent.
    /// Failure: permission_denied, io_failure.
    virtual Status remove_file(const std::filesystem::path& path) = 0;

    /// Lists the immediate entries of a directory, unsorted.
    /// Failure: not_found, permission_denied, io_failure.
    [[nodiscard]] virtual Result<std::vector<std::filesystem::path>> list_directory(
        const std::filesystem::path& path) const = 0;

    /// Reports free space on the volume holding `path`, for the "model cache is
    /// full" diagnostic. Failure: unsupported when the platform cannot answer.
    [[nodiscard]] virtual Result<std::uint64_t> available_space(const std::filesystem::path& path) const = 0;

protected:
    FileSystem() = default;
};

} // namespace voicetyper::platform
