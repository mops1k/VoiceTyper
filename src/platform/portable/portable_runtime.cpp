#include "platform/portable/portable_runtime.hpp"

#include <cerrno>

#ifdef _WIN32
#include <windows.h>
#endif
#include <chrono>
#include <fstream>
#include <system_error>
#include <thread>

namespace voicetyper::platform {
namespace {

Status errno_status(const char* what)
{
    const auto code = std::error_code(errno, std::generic_category());
    if (code.value() == EACCES || code.value() == EPERM) {
        return Status::failure(ErrorCode::permission_denied, what);
    }
    if (code.value() == ENOENT) {
        return Status::failure(ErrorCode::not_found, what);
    }
    return Status::failure(ErrorCode::io_failure, what);
}

namespace {

/// Atomic replace of `from` over `to`.
///
/// This cannot be std::filesystem::rename on Windows: the standard maps it to
/// MoveFile, which REFUSES an existing target, while POSIX rename replaces it.
/// A settings save that only works on the platform it was tested on is exactly
/// the kind of bug that hides until the primary target runs, so the Win32 branch
/// is explicit here.
Status replace_atomically(const std::filesystem::path& from, const std::filesystem::path& to)
{
#ifdef _WIN32
    const auto source = from.native();
    const auto target = to.native();
    if (!::MoveFileExW(source.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        const auto code = ::GetLastError();
        return Status::failure(
            ErrorCode::io_failure,
            "replace failed (win32 " + std::to_string(static_cast<unsigned long>(code)) + ")");
    }
    return Status::success();
#else
    std::error_code error;
    std::filesystem::rename(from, to, error);
    if (error) {
        return Status::failure(ErrorCode::io_failure, "replace failed: " + error.message());
    }
    return Status::success();
#endif
}

} // namespace

class PortableWriteStream final : public FileWriteStream {
public:
    // The temp path stays a std::filesystem::path: converting it to std::string
    // compiles under GCC but not under MinGW13, which is how a file that builds
    // on Arch fails on the primary target.
    PortableWriteStream(std::filesystem::path path, std::filesystem::path temp_path, bool append)
        : path_(std::move(path))
        , temp_path_(std::move(temp_path))
        , stream_(temp_path_, append ? std::ios::app : std::ios::trunc)
    {
    }

    Status write(const char* data, std::size_t size) override
    {
        if (!stream_) {
            return errno_status("could not open the file for writing");
        }
        stream_.write(data, static_cast<std::streamsize>(size));
        if (!stream_) {
            return Status::failure(ErrorCode::io_failure, "write failed");
        }
        written_ += size;
        return Status::success();
    }
    std::uint64_t bytes_written() const noexcept override { return written_; }
    Status flush() override
    {
        stream_.flush();
        return stream_ ? Status::success() : Status::failure(ErrorCode::io_failure, "flush failed");
    }
    void close() noexcept override
    {
        if (closed_) {
            return;
        }
        closed_ = true;
        stream_.close();
    }

private:
    std::filesystem::path path_;
    std::filesystem::path temp_path_;
    std::ofstream stream_;
    std::uint64_t written_ = 0;
    bool closed_ = false;
};

} // namespace

Status PortableClock::sleep_for(std::chrono::milliseconds duration, const CancellationToken& cancellation)
{
    const auto cancelled = check_cancelled(cancellation);
    if (cancelled.is_error()) {
        return cancelled;
    }
    if (duration.count() <= 0) {
        return Status::success();
    }
    // Slice the wait so cancellation is observed within ~20 ms, the same
    // granularity the Windows clock backend uses.
    auto remaining = duration;
    while (remaining.count() > 0) {
        const auto slice = std::min<std::chrono::milliseconds>(remaining, std::chrono::milliseconds(20));
        std::this_thread::sleep_for(slice);
        remaining -= slice;
        const auto now_cancelled = check_cancelled(cancellation);
        if (now_cancelled.is_error()) {
            return now_cancelled;
        }
    }
    return Status::success();
}

Status PortableClock::sleep_until(
    std::chrono::steady_clock::time_point deadline, const CancellationToken& cancellation)
{
    return sleep_for(
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()),
        cancellation);
}

bool PortableFileSystem::exists(const std::filesystem::path& path) const
{
    std::error_code error;
    return std::filesystem::exists(path, error);
}

Result<std::uint64_t> PortableFileSystem::file_size(const std::filesystem::path& path) const
{
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) {
        return Result<std::uint64_t>::failure(ErrorCode::not_found, error.message());
    }
    return static_cast<std::uint64_t>(size);
}

Status PortableFileSystem::create_directories(const std::filesystem::path& path)
{
    std::error_code error;
    std::filesystem::create_directories(path, error);
    if (error && !std::filesystem::exists(path)) {
        return errno_status("could not create the directory");
    }
    return Status::success();
}

Result<std::unique_ptr<FileWriteStream>> PortableFileSystem::open_write(
    const std::filesystem::path& path, FileOpenMode mode)
{
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        const auto created = create_directories(parent);
        if (created.is_error()) {
            return Result<std::unique_ptr<FileWriteStream>>::failure(created.code(), created.message());
        }
    }
    const auto append = mode == FileOpenMode::append;
    const auto temp = append ? path : std::filesystem::path(path).concat(".part");
    return std::unique_ptr<FileWriteStream>(new PortableWriteStream(path, temp, append));
}

Result<std::string> PortableFileSystem::read_text(const std::filesystem::path& path) const
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return Result<std::string>::failure(
            exists(path) ? ErrorCode::permission_denied : ErrorCode::not_found, "could not open the file");
    }
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

Result<std::vector<std::uint8_t>> PortableFileSystem::read_binary(const std::filesystem::path& path) const
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return Result<std::vector<std::uint8_t>>::failure(
            exists(path) ? ErrorCode::permission_denied : ErrorCode::not_found, "could not open the file");
    }
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

Status PortableFileSystem::atomic_write(
    const std::filesystem::path& path, std::string_view content, FileWriteMode mode)
{
    const auto temp = std::filesystem::path(path).concat(".tmp");
    if (mode == FileWriteMode::create_new && exists(path)) {
        return Status::failure(ErrorCode::already_exists, "the file already exists");
    }
    {
        std::ofstream stream(temp, std::ios::binary | std::ios::trunc);
        if (!stream) {
            return errno_status("could not open the temp file");
        }
        stream.write(content.data(), static_cast<std::streamsize>(content.size()));
        stream.flush();
        if (!stream) {
            stream.close();
            static_cast<void>(std::filesystem::remove(temp));
            return Status::failure(ErrorCode::io_failure, "write failed");
        }
    }
    const auto replaced = replace_atomically(temp, path);
    if (replaced.is_error()) {
        static_cast<void>(std::filesystem::remove(temp));
        return replaced;
    }
    return Status::success();
}

Status PortableFileSystem::replace_file(const std::filesystem::path& from, const std::filesystem::path& to)
{
    return replace_atomically(from, to);
}

Status PortableFileSystem::remove_file(const std::filesystem::path& path)
{
    std::error_code error;
    const auto removed = std::filesystem::remove(path, error);
    if (error) {
        return errno_status("could not remove the file");
    }
    if (removed == 0) {
        return Status::failure(ErrorCode::not_found, "the file was already absent");
    }
    return Status::success();
}

Result<std::vector<std::filesystem::path>> PortableFileSystem::list_directory(
    const std::filesystem::path& path) const
{
    std::error_code error;
    std::vector<std::filesystem::path> entries;
    for (const auto& entry : std::filesystem::directory_iterator(path, error)) {
        entries.push_back(entry.path());
    }
    if (error) {
        return Result<std::vector<std::filesystem::path>>::failure(ErrorCode::not_found, error.message());
    }
    return entries;
}

Result<std::uint64_t> PortableFileSystem::available_space(const std::filesystem::path& path) const
{
    std::error_code error;
    const auto space = std::filesystem::space(path, error);
    if (error) {
        return Result<std::uint64_t>::failure(ErrorCode::unsupported, "free space is unknown on this host");
    }
    return static_cast<std::uint64_t>(space.available);
}

} // namespace voicetyper::platform
