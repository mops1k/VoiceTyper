#include "platform/linux/linux_paths.hpp"

#include <unistd.h>

#include <cstddef>
#include <vector>

namespace voicetyper::platform::linuxos {
namespace {

/// readlink() truncates silently, so the buffer grows until the returned length
/// is shorter than the buffer: a path longer than the first guess must never be
/// reported as a shorter, wrong directory.
std::filesystem::path read_link(const char* link_path)
{
    std::size_t size = 1024;
    while (size <= 64 * 1024) {
        std::vector<char> buffer(size);
        const ssize_t length = ::readlink(link_path, buffer.data(), buffer.size() - 1);
        if (length < 0) {
            return {};
        }
        if (static_cast<std::size_t>(length) < buffer.size() - 1) {
            buffer[static_cast<std::size_t>(length)] = '\0';
            return std::filesystem::path(buffer.data());
        }
        size *= 2;
    }
    return {};
}

} // namespace

std::filesystem::path executable_file_path()
{
    // /proc/self/exe names the running image, so the answer is the real binary
    // even when the process was started through PATH or a symlink.
    return read_link("/proc/self/exe");
}

} // namespace voicetyper::platform::linuxos
