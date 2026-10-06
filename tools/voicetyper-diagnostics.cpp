#include "domain/version.hpp"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>

namespace {

void print_usage(std::ostream& out)
{
    out << "Usage: voicetyper-diagnostics [--help|--version|--json|--wav-info <file>]\n";
}

struct WavInfo {
    std::uint32_t sample_rate = 0;
    std::uint16_t channels = 0;
    std::uint16_t bits_per_sample = 0;
    std::uint64_t data_bytes = 0;
};

std::uint16_t read_u16(const char* data)
{
    return static_cast<std::uint16_t>(static_cast<unsigned char>(data[0])) |
        (static_cast<std::uint16_t>(static_cast<unsigned char>(data[1])) << 8);
}

std::uint32_t read_u32(const char* data)
{
    return static_cast<std::uint32_t>(static_cast<unsigned char>(data[0])) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(data[1])) << 8) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(data[2])) << 16) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(data[3])) << 24);
}

bool read_wav_info(const std::filesystem::path& path, WavInfo& info, std::string& error)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "cannot open WAV file";
        return false;
    }

    char header[12]{};
    if (!input.read(header, sizeof(header)) || std::memcmp(header, "RIFF", 4) != 0 ||
        std::memcmp(header + 8, "WAVE", 4) != 0) {
        error = "not a RIFF/WAVE file";
        return false;
    }

    bool saw_fmt = false;
    bool saw_data = false;
    while (true) {
        char chunk[8]{};
        if (!input.read(chunk, sizeof(chunk))) {
            break;
        }
        const auto size = read_u32(chunk + 4);
        const auto payload_start = static_cast<std::streamoff>(input.tellg());
        if (std::memcmp(chunk, "fmt ", 4) == 0) {
            if (size < 16) {
                error = "invalid fmt chunk size";
                return false;
            }
            char fmt[16]{};
            if (!input.read(fmt, sizeof(fmt))) {
                error = "truncated fmt chunk";
                return false;
            }
            if (read_u16(fmt) != 1) {
                error = "only PCM WAV is supported";
                return false;
            }
            info.channels = read_u16(fmt + 2);
            info.sample_rate = read_u32(fmt + 4);
            info.bits_per_sample = read_u16(fmt + 14);
            saw_fmt = true;
        } else if (std::memcmp(chunk, "data", 4) == 0) {
            info.data_bytes = size;
            saw_data = true;
        }
        const auto next = payload_start + static_cast<std::streamoff>(size) + static_cast<std::streamoff>(size & 1U);
        input.seekg(next);
        if (!input) {
            break;
        }
    }

    if (!saw_fmt || !saw_data || info.channels == 0 || info.sample_rate == 0 || info.bits_per_sample == 0) {
        error = "WAV is missing fmt/data or has an invalid format";
        return false;
    }
    return true;
}

int wav_info(const std::filesystem::path& path)
{
    WavInfo info;
    std::string error;
    if (!read_wav_info(path, info, error)) {
        std::cerr << "wav-info: " << error << ": " << path << '\n';
        return 1;
    }
    const auto frame_bytes = static_cast<std::uint32_t>(info.channels * (info.bits_per_sample / 8U));
    const auto frames = frame_bytes == 0 ? 0U : info.data_bytes / frame_bytes;
    const double duration_ms = frames == 0
        ? 0.0
        : (static_cast<double>(frames) * 1000.0) / static_cast<double>(info.sample_rate);
    std::cout << "{\"path\":\"" << path.string() << "\",\"sampleRate\":" << info.sample_rate
              << ",\"channels\":" << info.channels << ",\"bitsPerSample\":" << info.bits_per_sample
              << ",\"frames\":" << frames << ",\"durationMs\":" << std::fixed << std::setprecision(3)
              << duration_ms << "}\n";
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc == 2) {
        const std::string_view argument(argv[1]);
        if (argument == "--help" || argument == "-h") {
            print_usage(std::cout);
            return 0;
        }
        if (argument == "--version") {
            std::cout << voicetyper::domain::version() << '\n';
            return 0;
        }
        if (argument == "--json") {
            std::cout << "{\"name\":\"VoiceTyper\",\"version\":\""
                      << voicetyper::domain::version() << "\"}\n";
            return 0;
        }
    }
    if (argc == 3 && std::string_view(argv[1]) == "--wav-info") {
        return wav_info(std::filesystem::path(argv[2]));
    }

    print_usage(std::cerr);
    return 2;
}
