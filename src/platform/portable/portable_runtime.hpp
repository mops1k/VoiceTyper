#pragma once

// Portable fallbacks for the two platform seams the UI needs immediately:
// a FileSystem over <filesystem> and a Clock over <chrono>.
//
// They exist so the settings window and the composition root can be built and
// launched on any host, including a CI box with no Windows backends. On Windows
// the Win32 backends take over; these remain the correct choice for a portable
// contract build and for Linux, where std::filesystem is the native API anyway.

#include "platform/api/clock.hpp"
#include "platform/api/file_system.hpp"

#include <memory>
#include <vector>

namespace voicetyper::platform {

class PortableClock final : public Clock {
public:
    std::chrono::steady_clock::time_point now() const override { return std::chrono::steady_clock::now(); }
    std::chrono::system_clock::time_point wall_now() const override { return std::chrono::system_clock::now(); }
    std::chrono::steady_clock::duration elapsed_since(std::chrono::steady_clock::time_point start) const override
    {
        return std::chrono::steady_clock::now() - start;
    }
    Status sleep_for(std::chrono::milliseconds duration, const CancellationToken& cancellation) override;
    Status sleep_until(std::chrono::steady_clock::time_point deadline, const CancellationToken& cancellation) override;
};

class PortableFileSystem final : public FileSystem {
public:
    bool exists(const std::filesystem::path& path) const override;
    Result<std::uint64_t> file_size(const std::filesystem::path& path) const override;
    Status create_directories(const std::filesystem::path& path) override;
    Result<std::unique_ptr<FileWriteStream>> open_write(
        const std::filesystem::path& path, FileOpenMode mode) override;
    Result<std::string> read_text(const std::filesystem::path& path) const override;
    Result<std::vector<std::uint8_t>> read_binary(const std::filesystem::path& path) const override;
    Status atomic_write(
        const std::filesystem::path& path, std::string_view content, FileWriteMode mode = FileWriteMode::replace) override;
    Status replace_file(const std::filesystem::path& from, const std::filesystem::path& to) override;
    Status remove_file(const std::filesystem::path& path) override;
    Result<std::vector<std::filesystem::path>> list_directory(const std::filesystem::path& path) const override;
    Result<std::uint64_t> available_space(const std::filesystem::path& path) const override;
};

} // namespace voicetyper::platform
