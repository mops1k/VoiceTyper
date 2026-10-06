#include "domain/version.hpp"

#ifndef VOICETYPER_VERSION
#define VOICETYPER_VERSION "0.0.0-dev"
#endif

namespace voicetyper::domain {

std::string_view version() noexcept
{
    return VOICETYPER_VERSION;
}

} // namespace voicetyper::domain
