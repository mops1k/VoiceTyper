#pragma once

// Win32 implementation of the frozen platform::FileSystem contract.
//
// Evidence and contract:
//   * src/platform/api/file_system.hpp - the whole surface, including the new
//     open_write()/FileWriteStream streaming writer that a ~940 MB model
//     download cannot go through atomic_write()'s string_view payload.
//   * docs/migration/cpp/compatibility-contracts.md §1 (settings.json.tmp then
//     replace, no fsync, no transaction, no guaranteed temp cleanup) and §4
//     ("<target>.download", 128 KiB buffer, atomic-intended move, no fsync,
//     no checksum, no size enforcement, best-effort temp delete).
//   * src/platform/catalog_model_store.{hpp,cpp} - the consumer. It calls
//     open_write(FileOpenMode::truncate) for the temp, replace_file() to put the
//     finished download on its target, file_size() for the catalog check, and
//     list_directory() for the stale ".download" sweep.
//
// Deliberate non-strengthenings, all frozen by the contract:
//   * atomic_write() guarantees the temp-then-rename shape and nothing more.
//     No FlushFileBuffers and no MOVEFILE_WRITE_THROUGH: durability is a
//     deliberate stability decision that needs recovery tests first, and the
//     .NET reference performs no fsync at all.
//   * FileWriteStream::flush() is therefore a no-op that reports success, which
//     is exactly .NET's FileStream.Flush() (as opposed to Flush(true)). Every
//     write() already loops until the OS has taken all the bytes, so there is
//     no user-space buffer left to push.
//   * A failed write removes the temp file. The .NET build does not guarantee
//     that; the C++ settings codec already decided the same, and it is what
//     keeps a failed save from stranding settings.json.tmp forever.
//
// create_new is a hard precondition check, not a race-tolerant flag: when the
// target already exists the call fails with ErrorCode::already_exists *before*
// anything is written, so a create_new call can never truncate a file it was
// not allowed to replace.
//
// Error mapping is a single table (map_last_error) so every entry point reports
// the same code for the same Win32 condition. In particular a sharing violation
// is permission_denied, not a generic io_failure, and ERROR_FILE_NOT_FOUND is
// not_found rather than a silent false.
//
// Platform boundary: this header is standard-C++20 and includes no Windows
// header. The handles, MoveFileExW calls and FindFirstFileW enumeration live in
// win32_file_system.cpp.
//
// Thread affinity: safe from any thread. Nothing is cached between calls.
//
// Ownership: paths are values. open_write() returns an owned unique_ptr whose
// destructor closes the handle.

#include "domain/cancellation.hpp"
#include "domain/error.hpp"
#include "platform/api/file_system.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace voicetyper::platform {

using domain::CancellationToken;
using domain::ErrorCode;
using domain::Result;
using domain::Status;

/// Largest read_text() payload this backend will buffer. settings.json is a few
/// kilobytes; 64 MiB is far beyond any settings file and keeps a mis-pointed
/// path from turning into an out-of-memory kill. A larger file is reported as
/// ErrorCode::corrupt_data, which the contract reserves for "a size the backend
/// refuses to buffer".
inline constexpr std::uint64_t kMaxTextReadBytes = 64ull * 1024 * 1024;
/// Largest read_binary() payload. Model weights are validated by file_size(), not
/// by reading them, so this only has to cover WAV fixtures and small probe
/// files.
inline constexpr std::uint64_t kMaxBinaryReadBytes = 256ull * 1024 * 1024;
/// Suffix of the temporary file atomic_write() creates next to its target.
inline constexpr std::string_view kAtomicWriteTempSuffix = ".tmp";

/// A Win32 HANDLE-backed FileWriteStream.
///
/// Lifetime/ownership: the stream owns the handle, close() and the destructor
/// both release it exactly once.
class Win32FileWriteStream final : public FileWriteStream {
public:
    /// Takes ownership of an open Win32 handle. `mode` is kept only so the
    /// accessor can report how the file was opened; it does not change behaviour
    /// after construction.
    Win32FileWriteStream(void* handle, std::uint64_t existing_bytes, FileOpenMode mode);
    ~Win32FileWriteStream() override;

    Win32FileWriteStream(const Win32FileWriteStream&) = delete;
    Win32FileWriteStream& operator=(const Win32FileWriteStream&) = delete;
    Win32FileWriteStream(Win32FileWriteStream&&) = delete;
    Win32FileWriteStream& operator=(Win32FileWriteStream&&) = delete;

    /// Writes all `size` bytes, looping over short writes. A zero-length write
    /// is success. Failure: permission_denied, io_failure, resource_exhausted.
    Status write(const char* data, std::size_t size) override;

    /// Total bytes handed to the OS through this stream, including the bytes a
    /// reopened append file already contained.
    [[nodiscard]] std::uint64_t bytes_written() const noexcept override;

    /// Deliberately a no-op that reports success: see the durability note in
    /// this header. write() leaves nothing in a user-space buffer.
    Status flush() override;

    /// Closes the handle. Idempotent, and also run by the destructor.
    void close() noexcept override;

    /// The FileOpenMode this stream was opened with.
    [[nodiscard]] FileOpenMode mode() const noexcept;

private:
    void* handle_ = nullptr;
    std::uint64_t bytes_written_ = 0;
    FileOpenMode mode_ = FileOpenMode::truncate;
};

/// The Windows filesystem backend.
class Win32FileSystem final : public FileSystem {
public:
    Win32FileSystem() = default;
    ~Win32FileSystem() override = default;

    Win32FileSystem(const Win32FileSystem&) = delete;
    Win32FileSystem& operator=(const Win32FileSystem&) = delete;
    Win32FileSystem(Win32FileSystem&&) = delete;
    Win32FileSystem& operator=(Win32FileSystem&&) = delete;

    [[nodiscard]] bool exists(const std::filesystem::path& path) const override;

    /// Failure: not_found (missing path or not a regular file), permission_denied,
    /// io_failure.
    [[nodiscard]] Result<std::uint64_t> file_size(const std::filesystem::path& path) const override;

    /// Failure: permission_denied, not_found (an existing non-directory is in
    /// the way), io_failure.
    Status create_directories(const std::filesystem::path& path) override;

    /// truncate -> CREATE_ALWAYS, append -> OPEN_ALWAYS positioned at the end.
    /// Failure: not_found (a missing parent directory), permission_denied,
    /// resource_exhausted, io_failure.
    [[nodiscard]] Result<std::unique_ptr<FileWriteStream>> open_write(
        const std::filesystem::path& path, FileOpenMode mode) override;

    /// Failure: not_found, permission_denied, io_failure, corrupt_data.
    [[nodiscard]] Result<std::string> read_text(const std::filesystem::path& path) const override;

    /// Failure: not_found, permission_denied, io_failure, corrupt_data.
    [[nodiscard]] Result<std::vector<std::uint8_t>> read_binary(
        const std::filesystem::path& path) const override;

    /// Writes "<path>.tmp" and then moves it onto `path` with
    /// MoveFileExW(MOVEFILE_REPLACE_EXISTING). The temp is removed on every
    /// failure path, so a failed save never strands it.
    ///
    /// FileWriteMode::create_new refuses with ErrorCode::already_exists when the
    /// target is already there, before writing anything, and moves without the
    /// replace flag so a file created in between is not clobbered.
    ///
    /// Failure: already_exists, permission_denied, io_failure, resource_exhausted.
    Status atomic_write(
        const std::filesystem::path& path,
        std::string_view content,
        FileWriteMode mode = FileWriteMode::replace) override;

    /// MoveFileExW(REPLACE_EXISTING) from `from` onto `to`.
    /// Failure: not_found, permission_denied, already_exists (create semantics
    /// are not used here, so an existing `to` is replaced), io_failure.
    Status replace_file(const std::filesystem::path& from, const std::filesystem::path& to) override;

    /// DeleteFileW. Succeeds when the file was already absent.
    /// Failure: permission_denied, io_failure.
    Status remove_file(const std::filesystem::path& path) override;

    /// Immediate entries, unsorted, including hidden ones.
    /// Failure: not_found (no such directory), permission_denied, io_failure.
    [[nodiscard]] Result<std::vector<std::filesystem::path>> list_directory(
        const std::filesystem::path& path) const override;

    /// GetDiskFreeSpaceExW on the volume holding `path`.
    /// Failure: unsupported when the volume cannot be identified, not_found,
    /// permission_denied, io_failure.
    [[nodiscard]] Result<std::uint64_t> available_space(const std::filesystem::path& path) const override;
};

/// Maps a Win32 error to the stable domain code, exposed so the contract test
/// can assert the mapping table itself rather than only its effects.
[[nodiscard]] ErrorCode map_last_error(unsigned long win32_error) noexcept;

/// "<path>.tmp", the temp name atomic_write() uses.
[[nodiscard]] std::filesystem::path atomic_write_temp_path(const std::filesystem::path& path);

} // namespace voicetyper::platform
