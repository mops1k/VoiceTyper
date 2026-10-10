#include "platform/linux/linux_audio_capture.hpp"

#include "platform/linux/linux_microphone.hpp"
#include "platform/linux/pulse_support.hpp"

#include <pulse/error.h>
#include <pulse/simple.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <utility>

namespace voicetyper::platform::linuxos {
namespace {

/// How long start() waits for the sound server to open the source before it
/// gives up. The server answers in milliseconds when it is running, so this only
/// bounds the "no sound server" case.
constexpr auto kStartTimeout = std::chrono::milliseconds(3000);

/// The peak window the settings window's level meter reads (~0.2 s at 16 kHz).
constexpr std::size_t kLivePeakWindow = 8000;

} // namespace

LinuxAudioCapture::LinuxAudioCapture()
    : LinuxAudioCapture(Options{})
{
}

LinuxAudioCapture::LinuxAudioCapture(Options options)
    : options_(std::move(options))
{
}

LinuxAudioCapture::~LinuxAudioCapture()
{
    static_cast<void>(cancel());
}

Status LinuxAudioCapture::start(const CancellationToken& cancellation)
{
    if (cancellation.is_cancellation_requested()) {
        return Status::failure(ErrorCode::cancelled, "capture start cancelled");
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (recording_) {
            return Status::failure(ErrorCode::invalid_state, "a recording session is already running");
        }
        recording_ = true;
        stopping_ = false;
        overflowed_ = false;
        failed_ = false;
        start_finished_ = false;
        start_error_code_ = ErrorCode::unavailable;
        session_.clear();
        session_.shrink_to_fit();
        drained_ = 0;
        diagnostics_.clear();
        device_id_.clear();
        backend_.clear();
        device_format_ = AudioFormat();
    }

    capture_thread_ = std::thread([this] { capture_main(); });

    std::unique_lock<std::mutex> lock(mutex_);
    const auto deadline = std::chrono::steady_clock::now() + kStartTimeout;
    if (!ready_signal_.wait_until(lock, deadline, [this] { return start_finished_; })) {
        // The sound server never answered: stop the thread rather than leaving a
        // session that would deliver nothing.
        stopping_ = true;
        lock.unlock();
        if (capture_thread_.joinable()) {
            capture_thread_.join();
        }
        lock.lock();
        recording_ = false;
        return Status::failure(ErrorCode::unavailable,
            "the sound server did not open the source within "
                + std::to_string(kStartTimeout.count()) + " ms");
    }
    if (failed_) {
        const ErrorCode code = start_error_code_;
        const std::string detail = diagnostics_;
        lock.unlock();
        if (capture_thread_.joinable()) {
            capture_thread_.join();
        }
        lock.lock();
        recording_ = false;
        return Status::failure(code, detail);
    }
    return Status::success();
}

void LinuxAudioCapture::capture_main()
{
    // A source that does not exist has to be detected before the stream is
    // opened: measured 2026-10-08, pipewire-pulse answers pa_simple_new() with a
    // working stream even for a name that does not exist (the request silently
    // falls back), so the loader's own error is not a usable diagnostic. The
    // enumeration is the authoritative list of what can actually be opened.
    if (!options_.device_id.empty()) {
        LinuxMicrophoneService microphone;
        if (!microphone.is_device_available(options_.device_id)) {
            std::lock_guard<std::mutex> lock(mutex_);
            diagnostics_ = "no such capture source: " + options_.device_id
                + (microphone.diagnostics().empty() ? std::string()
                                                    : " (" + microphone.diagnostics() + ")");
            failed_ = true;
            start_error_code_ = ErrorCode::not_found;
            start_finished_ = true;
            ready_signal_.notify_all();
            return;
        }
    }

    pa_sample_spec spec{};
    spec.format = PA_SAMPLE_S16LE;
    spec.rate = options_.sample_rate;
    spec.channels = static_cast<std::uint8_t>(options_.channel_count);

    int error = 0;
    const char* device = options_.device_id.empty() ? nullptr : options_.device_id.c_str();
    pa_simple* stream = pa_simple_new(nullptr, kPulseClientName, PA_STREAM_RECORD, device,
        "dictation", &spec, nullptr, nullptr, &error);
    if (stream == nullptr) {
        std::lock_guard<std::mutex> lock(mutex_);
        diagnostics_ = pa_strerror(error);
        failed_ = true;
        // A source that does not exist is the case the settings page can act on;
        // everything else (no server, refused connection) is unavailable.
        start_error_code_ = error == PA_ERR_NOENTITY ? ErrorCode::not_found : ErrorCode::unavailable;
        start_finished_ = true;
        ready_signal_.notify_all();
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        device_format_ = AudioFormat(spec.rate, spec.channels, domain::SampleFormat::pcm_s16);
        device_id_ = options_.device_id;
        backend_ = std::string("pulse ") + std::to_string(spec.rate) + " Hz "
            + std::to_string(spec.channels) + " ch";
        start_finished_ = true;
        ready_signal_.notify_all();
    }

    // Ten milliseconds per read: pa_simple_read blocks until the buffer is full,
    // so a short block is what keeps stop() responsive without a second thread.
    const std::size_t frames_per_read = std::max<std::size_t>(1, spec.rate / 100);
    std::vector<std::int16_t> raw(frames_per_read * spec.channels);
    std::vector<float> converted(raw.size());

    while (true) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) {
                break;
            }
        }
        if (pa_simple_read(stream, raw.data(), raw.size() * sizeof(std::int16_t), &error) < 0) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!stopping_) {
                failed_ = true;
                diagnostics_ = pa_strerror(error);
            }
            break;
        }
        for (std::size_t index = 0; index < raw.size(); ++index) {
            converted[index] = static_cast<float>(raw[index]) / 32768.0F;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        // D7: the session is bounded. Once the bound is hit the audio is dropped
        // and stop() reports resource_exhausted instead of returning a silently
        // truncated recording.
        if (session_.size() + converted.size() > domain::kMaxRecordingBytes) {
            overflowed_ = true;
            continue;
        }
        session_.insert(session_.end(), converted.begin(), converted.end());
    }

    pa_simple_free(stream);
}

std::vector<float> LinuxAudioCapture::to_16k_mono_locked(std::size_t from, std::size_t to) const
{
    if (from >= to || to > session_.size() || device_format_.sample_rate() == 0) {
        return {};
    }
    const std::vector<float> slice(session_.begin() + static_cast<std::ptrdiff_t>(from),
        session_.begin() + static_cast<std::ptrdiff_t>(to));
    const auto channels = device_format_.channel_count();
    if (channels <= 1) {
        return domain::resample_to_16k(slice, device_format_.sample_rate());
    }
    if (channels == 2) {
        return domain::resample_to_16k(domain::downmix_to_mono(slice), device_format_.sample_rate());
    }
    // More than two channels is rare for a capture source; domain::downmix_to_mono
    // is specified as a stereo average, so the N-channel average is done here
    // with the same rule.
    std::vector<float> mono(slice.size() / channels);
    for (std::size_t frame = 0; frame < mono.size(); ++frame) {
        float sum = 0.0F;
        for (std::uint16_t channel = 0; channel < channels; ++channel) {
            sum += slice[frame * channels + channel];
        }
        mono[frame] = sum / static_cast<float>(channels);
    }
    return domain::resample_to_16k(mono, device_format_.sample_rate());
}

Result<SampleBuffer> LinuxAudioCapture::stop()
{
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!recording_) {
            return Result<SampleBuffer>::failure(ErrorCode::invalid_state, "no recording session");
        }
        stopping_ = true;
    }
    if (capture_thread_.joinable()) {
        capture_thread_.join();
    }

    std::lock_guard<std::mutex> lock(mutex_);
    recording_ = false;
    if (overflowed_) {
        session_.clear();
        session_.shrink_to_fit();
        drained_ = 0;
        return Result<SampleBuffer>::failure(ErrorCode::resource_exhausted,
            "the recording exceeded the 512 MiB bound and was not returned truncated");
    }
    if (failed_ && session_.empty()) {
        return Result<SampleBuffer>::failure(ErrorCode::io_failure, diagnostics_);
    }

    const std::vector<float> converted = to_16k_mono_locked(0, session_.size());
    session_.clear();
    session_.shrink_to_fit();
    drained_ = 0;

    SampleBuffer buffer;
    if (!converted.empty()) {
        const auto appended = buffer.append(converted);
        if (appended.is_error()) {
            return Result<SampleBuffer>::failure(appended.error().code(), appended.error().message());
        }
    }
    return buffer;
}

Status LinuxAudioCapture::cancel()
{
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!recording_) {
            // Idempotent: nothing to stop, nothing to clear, success.
            return Status::success();
        }
        stopping_ = true;
    }
    if (capture_thread_.joinable()) {
        capture_thread_.join();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    recording_ = false;
    session_.clear();
    session_.shrink_to_fit();
    drained_ = 0;
    return Status::success();
}

Result<SampleBuffer> LinuxAudioCapture::drain()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!recording_) {
        return Result<SampleBuffer>::failure(ErrorCode::invalid_state, "no recording session");
    }
    const std::vector<float> converted = to_16k_mono_locked(drained_, session_.size());
    drained_ = session_.size();

    SampleBuffer buffer;
    if (!converted.empty()) {
        const auto appended = buffer.append(converted);
        if (appended.is_error()) {
            return Result<SampleBuffer>::failure(appended.error().code(), appended.error().message());
        }
    }
    return buffer;
}

double LinuxAudioCapture::live_peak() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const std::size_t window = std::min<std::size_t>(session_.size(), kLivePeakWindow);
    double peak = 0.0;
    for (std::size_t index = session_.size() - window; index < session_.size(); ++index) {
        const double value = session_[index] < 0.0F ? -static_cast<double>(session_[index])
                                                   : static_cast<double>(session_[index]);
        if (value > peak) {
            peak = value;
        }
    }
    return peak;
}

bool LinuxAudioCapture::is_recording() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return recording_;
}

AudioFormat LinuxAudioCapture::device_format() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return device_format_;
}

std::string LinuxAudioCapture::device_id() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return device_id_;
}

std::string LinuxAudioCapture::backend_description() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return backend_;
}

std::string LinuxAudioCapture::diagnostics() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return diagnostics_;
}

bool LinuxAudioCapture::overflowed() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return overflowed_;
}

} // namespace voicetyper::platform::linuxos
