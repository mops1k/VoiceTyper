#pragma once

// Portable fallbacks for the platform seams the UI needs on any host, including
// CI: a FileSystem over <filesystem>, a Clock over <chrono>, and the no-op
// direct-delivery backend.
//
// They exist so the settings window and the composition root can be built and
// launched on any host, including a CI box with no Windows backends. On Windows
// the Win32 backends take over; these remain the correct choice for a portable
// contract build and for Linux, where std::filesystem is the native API anyway.
//
// The no-op text-target pair is what keeps VT-DOM-617 true on a host without a
// probe: `probe()` answers `unknown` and `insert()` refuses, so the delivery
// stays on the clipboard-then-paste path without special-casing in the caller.

#include "platform/api/clock.hpp"
#include "platform/api/file_system.hpp"
#include "platform/api/text_target.hpp"

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

/// No-op direct-delivery backend (VT-PLT-512). `probe()` cannot answer, which is
/// reported as `unknown` - never as `none`, because "we cannot ask" and "focus is
/// provably not in a text field" are different states.
class PortableTextTargetProbe final : public TextTargetProbe {
public:
    [[nodiscard]] TextTargetInfo probe() override;
    [[nodiscard]] bool is_supported() const noexcept override { return false; }
};

/// No-op typer: reports `unavailable` with `typed = 0`, so a caller that tries the
/// direct path first falls straight back to the clipboard with no partial
/// delivery to worry about.
class PortableDirectTextInjector final : public DirectTextInjector {
public:
    [[nodiscard]] InsertResult insert(
        std::string_view utf8, const CancellationToken& cancellation) override;
    [[nodiscard]] bool is_supported() const noexcept override { return false; }
};

/// No-op field reader (VT-PLT-516). `is_supported()` is false, so the domain keeps
/// its pre-existing behaviour instead of reporting every paste as unconfirmed: this
/// host has no accessibility reader yet, and a verdict it cannot make is not a
/// verdict.
class PortableFocusedTextField final : public FocusedTextField {
public:
    [[nodiscard]] bool read_caret_tail(std::size_t units, std::string* utf8) override;
    [[nodiscard]] bool read_value(std::string* utf8) override;
    /// Cannot answer here, so the caller stays silent (VT-PLT-518).
    [[nodiscard]] bool has_text_input_in_foreground_window() override { return true; }
    [[nodiscard]] bool is_supported() const noexcept override { return false; }
};

} // namespace voicetyper::platform
