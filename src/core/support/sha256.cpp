#include "core/support/sha256.hpp"

#include <array>
#include <cstdio>

namespace voicetyper::core::support {

namespace {

constexpr std::array<std::uint32_t, 64> kRoundConstants = {0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
    0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
    0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
    0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

[[nodiscard]] constexpr std::uint32_t rotate_right(std::uint32_t value, unsigned bits) noexcept
{
    return (value >> bits) | (value << (32U - bits));
}

[[nodiscard]] std::string to_hex(const std::uint8_t* bytes, std::size_t count)
{
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string text;
    text.reserve(count * 2);
    for (std::size_t index = 0; index < count; ++index) {
        text.push_back(kDigits[bytes[index] >> 4U]);
        text.push_back(kDigits[bytes[index] & 0x0FU]);
    }
    return text;
}

} // namespace

void Sha256::transform(const std::uint8_t* block)
{
    std::array<std::uint32_t, 64> schedule {};
    for (std::size_t index = 0; index < 16; ++index) {
        schedule[index] = (static_cast<std::uint32_t>(block[index * 4]) << 24U)
            | (static_cast<std::uint32_t>(block[index * 4 + 1]) << 16U)
            | (static_cast<std::uint32_t>(block[index * 4 + 2]) << 8U)
            | static_cast<std::uint32_t>(block[index * 4 + 3]);
    }
    for (std::size_t index = 16; index < 64; ++index) {
        const std::uint32_t s0 = rotate_right(schedule[index - 15], 7) ^ rotate_right(schedule[index - 15], 18)
            ^ (schedule[index - 15] >> 3U);
        const std::uint32_t s1 = rotate_right(schedule[index - 2], 17) ^ rotate_right(schedule[index - 2], 19)
            ^ (schedule[index - 2] >> 10U);
        schedule[index] = schedule[index - 16] + s0 + schedule[index - 7] + s1;
    }

    std::uint32_t a = state_[0];
    std::uint32_t b = state_[1];
    std::uint32_t c = state_[2];
    std::uint32_t d = state_[3];
    std::uint32_t e = state_[4];
    std::uint32_t f = state_[5];
    std::uint32_t g = state_[6];
    std::uint32_t h = state_[7];

    for (std::size_t index = 0; index < 64; ++index) {
        const std::uint32_t s1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
        const std::uint32_t choice = (e & f) ^ (~e & g);
        const std::uint32_t temp1 = h + s1 + choice + kRoundConstants[index] + schedule[index];
        const std::uint32_t s0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
        const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t temp2 = s0 + majority;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
}

void Sha256::update(std::span<const std::uint8_t> data)
{
    length_ += data.size();
    std::size_t offset = 0;
    if (buffered_ > 0) {
        while (offset < data.size() && buffered_ < sizeof(buffer_)) {
            buffer_[buffered_++] = data[offset++];
        }
        if (buffered_ == sizeof(buffer_)) {
            transform(buffer_);
            buffered_ = 0;
        }
    }
    while (offset + sizeof(buffer_) <= data.size()) {
        transform(data.data() + offset);
        offset += sizeof(buffer_);
    }
    while (offset < data.size()) {
        buffer_[buffered_++] = data[offset++];
    }
}

std::string Sha256::finish_hex()
{
    const std::uint64_t bits = length_ * 8U;
    const std::size_t original_buffered = buffered_;
    // 0x80, then zeros, then the 64-bit length: the standard padding.
    buffer_[buffered_++] = 0x80U;
    if (buffered_ > 56) {
        while (buffered_ < sizeof(buffer_)) {
            buffer_[buffered_++] = 0;
        }
        transform(buffer_);
        buffered_ = 0;
    }
    while (buffered_ < 56) {
        buffer_[buffered_++] = 0;
    }
    for (int shift = 56; shift >= 0; shift -= 8) {
        buffer_[buffered_++] = static_cast<std::uint8_t>((bits >> static_cast<unsigned>(shift)) & 0xFFU);
    }
    transform(buffer_);
    static_cast<void>(original_buffered);

    std::uint8_t digest[32] = {};
    for (std::size_t index = 0; index < 8; ++index) {
        digest[index * 4] = static_cast<std::uint8_t>(state_[index] >> 24U);
        digest[index * 4 + 1] = static_cast<std::uint8_t>(state_[index] >> 16U);
        digest[index * 4 + 2] = static_cast<std::uint8_t>(state_[index] >> 8U);
        digest[index * 4 + 3] = static_cast<std::uint8_t>(state_[index]);
    }
    return to_hex(digest, sizeof(digest));
}

std::string sha256_hex(std::span<const std::uint8_t> data)
{
    Sha256 hasher;
    hasher.update(data);
    return hasher.finish_hex();
}

std::string sha256_hex(std::string_view text)
{
    return sha256_hex(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

} // namespace voicetyper::core::support
