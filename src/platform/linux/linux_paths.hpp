#pragma once

// Linux executable location: the equivalent of the Win32 module_file_path().
//
// The composition root needs the directory of the running binary for two
// decisions: where the native engine libraries are looked for first, and the
// fallback root when the environment has no HOME at all. On Linux the running
// image is named by /proc/self/exe, which is a symlink to the real path, so the
// answer is the actual binary even when it was started through a PATH lookup or
// a symlink.
//
// This header is standard C++ only; the readlink() call lives in the .cpp.

#include <filesystem>

namespace voicetyper::platform::linuxos {

/// Absolute path of the running executable, empty when it cannot be resolved
/// (a kernel without /proc, a hardened sandbox). Callers fall back to
/// std::filesystem::current_path() exactly as the Windows composition does.
[[nodiscard]] std::filesystem::path executable_file_path();

} // namespace voicetyper::platform::linuxos
