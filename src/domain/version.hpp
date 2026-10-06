#pragma once

#include <string_view>

namespace voicetyper::domain {

/// Returns the build version supplied by the build system.
[[nodiscard]] std::string_view version() noexcept;

} // namespace voicetyper::domain
