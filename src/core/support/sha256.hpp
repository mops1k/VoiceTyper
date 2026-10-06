#pragma once

// SHA-256 (FIPS 180-4), needed to verify a downloaded installer against the hash the
// release body carries (plan p_a95b558c6861, Phase 3: the .NET UpdateService verified
// the SHA-256 marker before running the installer).
//
// Written here rather than pulled in as a dependency: the project adds no third-party
// libraries, and a download check must not be weaker than the one it replaces. The
// streaming form exists because the installer is 60+ MB and is hashed while it is
// written, not after being read back into memory.

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace voicetyper::core::support {

/// Lowercase hexadecimal SHA-256.
[[nodiscard]] std::string sha256_hex(std::span<const std::uint8_t> data);
[[nodiscard]] std::string sha256_hex(std::string_view text);

/// Incremental hasher: update() with as many chunks as needed, then finish_hex() once.
class Sha256 {
public:
    Sha256() = default;

    void update(std::span<const std::uint8_t> data);
    /// Finalises and returns the digest. The hasher must not be reused afterwards.
    [[nodiscard]] std::string finish_hex();

private:
    void transform(const std::uint8_t* block);

    std::uint32_t state_[8] = {0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
    std::uint64_t length_ = 0;
    std::uint8_t buffer_[64] = {};
    std::size_t buffered_ = 0;
};

} // namespace voicetyper::core::support
