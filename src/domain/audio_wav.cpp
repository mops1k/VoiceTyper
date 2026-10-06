#include "domain/audio_wav.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>
#include <system_error>
#include <utility>

namespace voicetyper::domain {
namespace {

std::uint16_t read_u16(std::string_view bytes, std::size_t offset)
{
    return static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[offset])) |
        (static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[offset + 1])) << 8);
}

std::uint32_t read_u32(std::string_view bytes, std::size_t offset)
{
    return static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset])) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 1])) << 8) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 2])) << 16) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 3])) << 24);
}

void append_u16(std::string& output, std::uint16_t value)
{
    output.push_back(static_cast<char>(value & 0xFF));
    output.push_back(static_cast<char>((value >> 8) & 0xFF));
}

void append_u32(std::string& output, std::uint32_t value)
{
    output.push_back(static_cast<char>(value & 0xFF));
    output.push_back(static_cast<char>((value >> 8) & 0xFF));
    output.push_back(static_cast<char>((value >> 16) & 0xFF));
    output.push_back(static_cast<char>((value >> 24) & 0xFF));
}

Result<std::vector<float>> unsupported(const char* message)
{
    return Result<std::vector<float>>::failure(ErrorCode::unsupported, message);
}

} // namespace

SampleBuffer::SampleBuffer(std::size_t capacity)
    : capacity_(capacity)
{
    samples_.reserve(std::min(capacity, kDefaultAudioCapacity));
}

Status SampleBuffer::append(const float* samples, std::size_t count)
{
    if (samples == nullptr && count != 0) {
        return Status::failure(ErrorCode::invalid_argument, "audio sample pointer is null");
    }
    if (count > capacity_ - std::min(capacity_, samples_.size())) {
        return Status::failure(ErrorCode::resource_exhausted, "audio buffer capacity exceeded");
    }
    samples_.insert(samples_.end(), samples, samples + count);
    return Status::success();
}

Status SampleBuffer::append(const std::vector<float>& samples)
{
    return append(samples.data(), samples.size());
}

Status SampleBuffer::append_d7_bounded(const float* samples, std::size_t count)
{
    if (count > (kMaxRecordingBytes / sizeof(float))) {
        return Status::failure(ErrorCode::resource_exhausted, "D7 recording byte bound exceeded");
    }
    const auto current_bytes = samples_.size() * sizeof(float);
    if (current_bytes > kMaxRecordingBytes - count * sizeof(float)) {
        return Status::failure(ErrorCode::resource_exhausted, "D7 recording byte bound exceeded");
    }
    return append(samples, count);
}

Result<std::vector<float>> read_wav_pcm16(std::span<const std::byte> bytes)
{
    return read_wav_pcm16(
        std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

Result<std::vector<float>> read_wav_pcm16(std::string_view bytes)
{
    if (bytes.size() < 12 || bytes.substr(0, 4) != "RIFF" || bytes.substr(8, 4) != "WAVE") {
        return unsupported("not a RIFF/WAVE container");
    }

    bool fmt_ok = false;
    std::uint16_t audio_format = 0;
    std::uint16_t channels = 0;
    std::uint16_t bits = 0;
    std::uint32_t sample_rate = 0;
    std::size_t data_start = 0;
    std::size_t data_size = 0;
    bool data_ok = false;

    std::size_t position = 12;
    while (position + 8 <= bytes.size()) {
        const auto chunk_size = read_u32(bytes, position + 4);
        const auto payload = position + 8;
        if (chunk_size > bytes.size() - payload) {
            break;
        }
        if (bytes.substr(position, 4) == "fmt ") {
            if (chunk_size < 16) {
                return unsupported("fmt chunk is too small");
            }
            audio_format = read_u16(bytes, payload);
            channels = read_u16(bytes, payload + 2);
            sample_rate = read_u32(bytes, payload + 4);
            bits = read_u16(bytes, payload + 14);
            fmt_ok = true;
        } else if (bytes.substr(position, 4) == "data") {
            data_start = payload;
            data_size = chunk_size;
            data_ok = true;
        }
        const auto next = payload + chunk_size + (chunk_size & 1U);
        if (next <= position || next > bytes.size()) {
            break;
        }
        position = next;
    }

    if (!fmt_ok || !data_ok) {
        return unsupported("WAV must contain fmt and data chunks");
    }
    if (audio_format != 1) {
        return unsupported("PCM audioFormat=1 is required");
    }
    if (bits != 16) {
        return unsupported("PCM16 bitsPerSample=16 is required");
    }
    if (channels != 1) {
        return unsupported("mono WAV is required");
    }
    if (sample_rate != kTargetSampleRate) {
        return unsupported("16 kHz WAV is required");
    }

    std::vector<float> result;
    result.reserve(data_size / 2);
    for (std::size_t i = 0; i + 1 < data_size; i += 2) {
        const auto sample = static_cast<std::int16_t>(read_u16(bytes, data_start + i));
        result.push_back(static_cast<float>(sample) / 32768.0f);
    }
    return result;
}

Result<std::vector<float>> read_wav_file(const std::string& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return Result<std::vector<float>>::failure(ErrorCode::not_found, "WAV file cannot be opened");
    }
    const std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    return read_wav_pcm16(bytes);
}

Status write_wav_pcm16(
    const std::vector<float>& samples,
    std::string& bytes,
    std::uint32_t sample_rate)
{
    if (sample_rate != kTargetSampleRate) {
        return Status::failure(ErrorCode::invalid_argument, "WAV writer requires 16 kHz");
    }
    if (samples.size() > std::numeric_limits<std::uint32_t>::max() / 2U) {
        return Status::failure(ErrorCode::resource_exhausted, "WAV sample count exceeds uint32");
    }
    const auto data_bytes = static_cast<std::uint32_t>(samples.size() * 2U);
    bytes.clear();
    bytes.reserve(44U + data_bytes);
    bytes.append("RIFF", 4);
    append_u32(bytes, 36U + data_bytes);
    bytes.append("WAVE", 4);
    bytes.append("fmt ", 4);
    append_u32(bytes, 16);
    append_u16(bytes, 1);
    append_u16(bytes, 1);
    append_u32(bytes, sample_rate);
    append_u32(bytes, sample_rate * 2U);
    append_u16(bytes, 2);
    append_u16(bytes, 16);
    bytes.append("data", 4);
    append_u32(bytes, data_bytes);
    for (const auto sample : samples) {
        const auto clamped = std::clamp(sample, -1.0f, 1.0f);
        const auto scaled = static_cast<int>(std::lround(static_cast<double>(clamped) * 32767.0));
        append_u16(bytes, static_cast<std::uint16_t>(static_cast<std::int16_t>(scaled)));
    }
    return Status::success();
}

std::vector<float> resample_to_16k(const std::vector<float>& samples, std::uint32_t source_rate)
{
    if (samples.empty() || source_rate == 0) {
        return {};
    }
    if (source_rate == kTargetSampleRate) {
        return samples;
    }
    const auto source = static_cast<double>(source_rate);
    const auto target = static_cast<double>(kTargetSampleRate);
    const auto count = static_cast<std::size_t>(
        std::ceil(static_cast<double>(samples.size()) * target / source));
    std::vector<float> result;
    result.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto position = static_cast<double>(i) * source / target;
        const auto left = static_cast<std::size_t>(position);
        const auto right = std::min(left + 1, samples.size() - 1);
        const auto fraction = static_cast<float>(position - static_cast<double>(left));
        result.push_back(samples[left] + (samples[right] - samples[left]) * fraction);
    }
    return result;
}

std::vector<float> downmix_to_mono(const std::vector<float>& interleaved_stereo)
{
    std::vector<float> result;
    result.reserve(interleaved_stereo.size() / 2);
    for (std::size_t i = 0; i + 1 < interleaved_stereo.size(); i += 2) {
        result.push_back((interleaved_stereo[i] + interleaved_stereo[i + 1]) * 0.5f);
    }
    return result;
}

void suppress_noise(std::vector<float>& samples, std::size_t sample_rate)
{
    (void)sample_rate; // The C# constants are defined for the 16 kHz pipeline.
    constexpr std::size_t frame_size = 256;
    constexpr double noise_floor_init = 1e-4;
    constexpr double threshold_ratio = 2.0;
    constexpr double damp_gain = 0.6;
    double noise_floor = noise_floor_init;
    double gain = 1.0;
    double lp_x = 0.0;
    double lp_y = 0.0;
    for (std::size_t start = 0; start < samples.size(); start += frame_size) {
        const auto count = std::min(frame_size, samples.size() - start);
        double energy = 0.0;
        for (std::size_t i = 0; i < count; ++i) {
            const auto x = static_cast<double>(samples[start + i]);
            const auto y = 0.94 * (lp_y + x - lp_x);
            lp_x = x;
            lp_y = y;
            samples[start + i] = static_cast<float>(y);
            energy += y * y;
        }
        energy /= static_cast<double>(count);
        noise_floor = energy < noise_floor
            ? energy
            : noise_floor * 1.02 + energy * 0.001;
        const auto is_speech = energy > noise_floor * threshold_ratio;
        const auto target = is_speech ? 1.0 : damp_gain;
        gain = gain * 0.7 + target * 0.3;
        if (gain < 0.999) {
            for (std::size_t i = 0; i < count; ++i) {
                samples[start + i] *= static_cast<float>(gain);
            }
        }
    }
}

std::vector<float> trim_silence(
    const std::vector<float>& samples,
    std::size_t sample_rate,
    double margin_seconds,
    double max_silence_seconds,
    double gap_silence_seconds)
{
    constexpr std::size_t frame_size = 160;
    if (samples.size() < frame_size) {
        return samples;
    }
    const auto frame_count = samples.size() / frame_size;
    std::vector<double> rms(frame_count);
    for (std::size_t frame = 0; frame < frame_count; ++frame) {
        double sum = 0.0;
        for (std::size_t i = 0; i < frame_size; ++i) {
            const auto value = static_cast<double>(samples[frame * frame_size + i]);
            sum += value * value;
        }
        rms[frame] = std::sqrt(sum / frame_size);
    }
    auto sorted = rms;
    std::sort(sorted.begin(), sorted.end());
    const auto percentile_index = static_cast<std::size_t>(sorted.size() * 0.15);
    const auto noise_floor = sorted[std::min(sorted.size() - 1, percentile_index)];
    const auto adaptive = std::max(noise_floor * 3.0, 1e-6);
    const bool any_active = std::any_of(rms.begin(), rms.end(), [adaptive](double value) { return value > adaptive; });
    const auto threshold = any_active ? static_cast<float>(adaptive) : 0.005f;

    std::vector<bool> active(frame_count);
    for (std::size_t frame = 0; frame < frame_count; ++frame) {
        active[frame] = rms[frame] >= threshold;
    }
    const auto first = std::find(active.begin(), active.end(), true);
    if (first == active.end()) {
        return {};
    }
    const auto last = std::find(active.rbegin(), active.rend(), true);
    const auto first_index = static_cast<std::size_t>(first - active.begin());
    const auto last_index = static_cast<std::size_t>(std::distance(active.begin(), last.base())) - 1;
    const auto margin = static_cast<std::size_t>(margin_seconds * static_cast<double>(sample_rate));
    const auto max_silence = static_cast<std::size_t>(max_silence_seconds * static_cast<double>(sample_rate));
    const auto gap_silence = static_cast<std::size_t>(gap_silence_seconds * static_cast<double>(sample_rate));
    const auto start = first_index * frame_size > margin ? first_index * frame_size - margin : 0;
    const auto end = std::min(samples.size(), (last_index + 1) * frame_size + margin);

    std::vector<float> result;
    std::size_t i = start;
    while (i < end) {
        const auto frame = i / frame_size;
        if (active[frame]) {
            result.push_back(samples[i++]);
            continue;
        }
        const auto silence_start = i;
        while (i < end && !active[i / frame_size]) {
            ++i;
        }
        const auto silence_length = i - silence_start;
        const auto keep = silence_length > max_silence ? gap_silence : silence_length;
        for (std::size_t k = 0; k < keep && silence_start + k < end; ++k) {
            result.push_back(samples[silence_start + k]);
        }
    }
    return result;
}

} // namespace voicetyper::domain
