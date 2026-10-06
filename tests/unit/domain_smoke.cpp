#include "domain/version.hpp"

#include <string>

int main()
{
    const std::string version(voicetyper::domain::version());
    return version.empty() ? 1 : 0;
}
