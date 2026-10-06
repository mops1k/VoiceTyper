// WindowsAudioCapture: the RecordingPort adapter that turns WASAPI packets into
// the 16 kHz mono float buffer the recording state machine hands to the
// transcriber. Companion to windows_audio_capture.hpp, which documents the
// contract; this file contains the conversion and the stop ordering.
//
// The three rules that are easy to get wrong and are therefore implemented
// literally, in this order, in stop():
//   1. device: the capture backend stops the client and joins its thread;
//   2. unsubscribe: the backend drops the packet sink;
//   3. snapshot: only now is the session buffer copied, converted and cleared.
// A conversion error can therefore never race the capture thread, and a cleared
// buffer is never observable to the caller.

#include "platform/windows/windows_audio_capture.hpp"

#include <cstring>
#include <utility>

namespace voicetyper::platform {
namespace {

/// PCM16 -> float, matching the /32768 scaling of domain::read_wav_pcm16 so a
/// capture and a WAV round-trip of the same bytes produce identical samples.
constexpr float kPcm16Scale = 1.0f / 32768.0f;

/// D7: one recording is bounded at 512 MiB of float samples, the same bound
/// domain::SampleBuffer enforces.
constexpr std::size_t kMaxSessionSamples = domain::kMaxRecordingBytes / sizeof(float);

/// An empty id and the legacy "0" both mean "the system default endpoint"
/// (RawWasapiCapture.ResolveDevice). Repeated here instead of calling
/// platform::is_default_microphone_id so that this translation unit does not
/// depend on the microphone backend.
bool selects_default_endpoint(const std::string& device_id)
{
    return device_id.empty() || device_id == "0";
}

/// Reads `count` PCM16 values. WAVEFORMATEX is little-endian and so is every
/// Windows target, so a direct copy of the two bytes is exact.
void append_pcm16(std::vector<float>& out, const std::byte* data, std::size_t count)
{
    out.reserve(out.size() + count);
    for (std::size_t i = 0; i < count; ++i) {
        std::int16_t value = 0;
        std::memcpy(&value, data + i * sizeof(std::int16_t), sizeof(value));
        out.push_back(static_cast<float>(value) * kPcm16Scale);
    }
}

void append_float32(std::vector<float>& out, const std::byte* data, std::size_t count)
{
    out.reserve(out.size() + count);
    for (std::size_t i = 0; i < count; ++i) {
        float value = 0.0f;
        std::memcpy(&value, data + i * sizeof(float), sizeof(value));
        out.push_back(value);
    }
}

domain::SampleBuffer make_sample_buffer(const std::vector<float>& samples)
{
    domain::SampleBuffer buffer(samples.size());
    (void)buffer.append(samples);
    return buffer;
}

} // namespace

WindowsAudioCapture::WindowsAudioCapture()
    : WindowsAudioCapture(Options {})
{
}

WindowsAudioCapture::WindowsAudioCapture(Options options)
    : options_(std::move(options))
{
    start_idle_reaper();
}

WindowsAudioCapture::~WindowsAudioCapture()
{
    {
        std::lock_guard<std::mutex> lock(idle_mutex_);
        idle_reaper_stop_ = true;
    }
    idle_cv_.notify_all();
    if (idle_reaper_.joinable()) {
        idle_reaper_.join();
    }
    // The backend releases its COM objects in its own destructor; nothing may
    // escape a destructor, so a failure here is dropped on purpose.
    (void)cancel();
}

bool WindowsAudioCapture::is_recording() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return recording_;
}

domain::AudioFormat WindowsAudioCapture::device_format() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return device_format_;
}

std::string WindowsAudioCapture::device_id() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return device_id_;
}

std::string WindowsAudioCapture::backend_description() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return backend_;
}

std::string WindowsAudioCapture::diagnostics() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return diagnostics_;
}

WasapiEndReason WindowsAudioCapture::end_reason() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return end_reason_;
}

bool WindowsAudioCapture::overflowed() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return overflowed_;
}

void WindowsAudioCapture::start_idle_reaper()
{
    {
        std::lock_guard<std::mutex> lock(idle_mutex_);
        last_activity_ = std::chrono::steady_clock::now();
    }
    idle_reaper_ = std::thread([this] {
        std::unique_lock<std::mutex> lock(idle_mutex_);
        while (!idle_reaper_stop_) {
            idle_cv_.wait_for(lock, std::chrono::seconds(5));
            if (idle_reaper_stop_) {
                return;
            }
            if (std::chrono::steady_clock::now() - last_activity_ < kIdleReleaseDelay) {
                continue;
            }
            lock.unlock();
            {
                std::lock_guard<std::mutex> session_lock(mutex_);
                // Never while a session is live: the client is the session.
                if (!recording_ && capture_ != nullptr) {
                    capture_->destroy();
                    capture_.reset();
                }
            }
            lock.lock();
            last_activity_ = std::chrono::steady_clock::now();
        }
    });
}

void WindowsAudioCapture::note_session_stopped()
{
    {
        std::lock_guard<std::mutex> lock(idle_mutex_);
        last_activity_ = std::chrono::steady_clock::now();
    }
    idle_cv_.notify_all();
}

void WindowsAudioCapture::append_packet(const WasapiPacket& packet)
{
    if (packet.data == nullptr || packet.bytes == 0) {
        return;
    }
    const domain::AudioFormat format = packet.format;
    if (format.validate().is_error()) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!recording_ || overflowed_) {
        return;
    }
    // The packet carries its own format, so the first packet of a session is
    // parsed with the format the device really delivers even when it arrives
    // before start() has returned to the application thread.
    device_format_ = format;
    const std::size_t count = packet.bytes / format.bytes_per_sample();
    if (count == 0 || session_.size() + count > kMaxSessionSamples) {
        overflowed_ = true;
        return;
    }
    if (format.sample_format() == domain::SampleFormat::ieee_float32) {
        append_float32(session_, packet.data, count);
    } else {
        append_pcm16(session_, packet.data, count);
    }
}

std::vector<float> WindowsAudioCapture::to_16k_mono_locked(std::size_t from, std::size_t to) const
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
    // More than two channels is rare for capture endpoints (a multi-mic array
    // can expose 4 or 6). domain::downmix_to_mono is specified as a stereo
    // average, so the N-channel average is done here with the same rule.
    std::vector<float> mono(slice.size() / channels);
    for (std::size_t frame = 0; frame < mono.size(); ++frame) {
        float sum = 0.0f;
        for (std::uint16_t channel = 0; channel < channels; ++channel) {
            sum += slice[frame * channels + channel];
        }
        mono[frame] = sum / static_cast<float>(channels);
    }
    return domain::resample_to_16k(mono, device_format_.sample_rate());
}

Status WindowsAudioCapture::start(const domain::CancellationToken& cancellation)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (recording_) {
            return Status::failure(
                domain::ErrorCode::invalid_state, "a recording session is already running");
        }
        session_.clear();
        session_.shrink_to_fit();
        drained_ = 0;
        overflowed_ = false;
        diagnostics_.clear();
        device_id_.clear();
        backend_.clear();
        device_format_ = domain::AudioFormat();
        end_reason_ = WasapiEndReason::completed;
        // Opened before the device is: a packet can arrive before start()
        // returns, and dropping it would lose the first few milliseconds of
        // every recording. Each packet carries its own format, so the buffer is
        // correct even in that window.
        recording_ = true;
    }

    // The native library first. This is the path the .NET build proved on this
    // hardware: on the Intel Smart Sound array the interop WASAPI path answers
    // E_INVALIDARG for every format and every endpoint, while mc_wasapi.dll opens
    // the device (EXCLUSIVE PCM16 48 kHz stereo). See platform/mc_wasapi.hpp.
    if (!mc_api_.available()) {
        mc_api_ = load_mc_wasapi();
    }
    std::string native_failure;
    native_buffers_.store(0, std::memory_order_relaxed);
    native_bytes_.store(0, std::memory_order_relaxed);
    if (mc_api_.available()) {
        auto session = std::make_unique<McWasapiSession>(
            mc_api_, options_.sample_rate, options_.channel_count);
        const Status native_started = session->start(
            [this](const void* data, int bytes, int rate, int channels) {
                native_buffers_.fetch_add(1, std::memory_order_relaxed);
                native_bytes_.fetch_add(static_cast<std::size_t>(bytes), std::memory_order_relaxed);
                WasapiPacket packet;
                packet.data = static_cast<const std::byte*>(data);
                packet.bytes = static_cast<std::size_t>(bytes);
                packet.format = domain::AudioFormat(static_cast<std::uint32_t>(rate),
                    static_cast<std::uint16_t>(channels), domain::SampleFormat::pcm_s16);
                append_packet(packet);
            });
        if (native_started.is_ok()) {
            std::lock_guard<std::mutex> lock(mutex_);
            mc_session_ = std::move(session);
            device_format_ = domain::AudioFormat(
                options_.sample_rate, options_.channel_count, domain::SampleFormat::pcm_s16);
            backend_ = "mc_wasapi.dll native " + std::to_string(options_.sample_rate) + "Hz "
                + std::to_string(options_.channel_count) + "ch PCM16";
            device_id_ = options_.device_id;
            return Status::success();
        }
        native_failure = native_started.error().to_string();
    }

    // The session object is kept between recordings on purpose: it holds the
    // negotiated IAudioClient, and reusing it is what makes every dictation after
    // the first start immediately instead of paying the ~1 s negotiation again.
    std::unique_ptr<WasapiCapture> capture;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        capture = std::move(capture_);
    }
    if (capture == nullptr) {
        auto created = WasapiCapture::create();
        if (created.is_error()) {
            std::lock_guard<std::mutex> lock(mutex_);
            recording_ = false;
            return Status::failure(created.error());
        }
        capture = std::move(created).value();
    }

    WasapiCaptureOptions capture_options;
    capture_options.device_id = selects_default_endpoint(options_.device_id) ? std::string()
                                                                            : options_.device_id;
    capture_options.strategy = options_.strategy;
    capture_options.sample_rate = options_.sample_rate;
    capture_options.channel_count = options_.channel_count;

    const Status started = capture->start(
        capture_options, [this](const WasapiPacket& packet) { append_packet(packet); }, cancellation);
    if (started.is_error()) {
        const std::string detail = capture->report().diagnostics;
        const domain::Error error = started.error();
        std::lock_guard<std::mutex> lock(mutex_);
        recording_ = false;
        session_.clear();
        drained_ = 0;
        diagnostics_ = error.detail().empty() ? detail : error.detail();
        if (!native_failure.empty()) {
            diagnostics_ = "native capture: " + native_failure + "; WASAPI: " + diagnostics_;
        }
        return Status::failure(domain::Error(error.code(), error.message(), diagnostics_));
    }

    const WasapiCaptureReport report = capture->report();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        device_format_ = capture->device_format();
        device_id_ = report.device_id;
        diagnostics_ = report.diagnostics;
        backend_ = std::string("WASAPI ") + std::string(wasapi_share_mode_name(report.share_mode)) + " "
            + (report.event_driven ? "event" : "poll") + " "
            + std::to_string(report.buffer_milliseconds) + "ms";
        capture_ = std::move(capture);
    }
    return Status::success();
}

Result<domain::SampleBuffer> WindowsAudioCapture::stop()
{
    std::unique_ptr<WasapiCapture> capture;
    std::unique_ptr<McWasapiSession> native_session;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!recording_) {
            return Result<domain::SampleBuffer>::failure(
                domain::ErrorCode::invalid_state, "no recording session is running");
        }
        recording_ = false;
        native_session = std::move(mc_session_);
        if (native_session == nullptr) {
            capture = std::move(capture_);
        }
    }

    if (native_session != nullptr) {
        // The native path owns the device: the WASAPI session object stays where it
        // is (it may hold a client from an earlier dictation) and the backend keeps
        // the description start() wrote.
        native_session->stop();
        std::lock_guard<std::mutex> lock(mutex_);
        backend_ += " buffers=" + std::to_string(native_buffers_.load(std::memory_order_relaxed))
            + " bytes=" + std::to_string(native_bytes_.load(std::memory_order_relaxed));
        end_reason_ = WasapiEndReason::completed;
        note_session_stopped();
    }

    // 1. device + 2. unsubscribe happen here, outside the session mutex, so the
    //    capture thread can still take that mutex from its packet callback while
    //    the backend shuts down.
    if (capture != nullptr) {
        const Status stopped = capture->stop();
        const WasapiCaptureReport report = capture->report();
        const WasapiEndReason reason = capture->end_reason();
        // Note: no destroy() here - the negotiated client stays alive for the next
        // dictation, and ~WindowsAudioCapture() destroys it for real.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            capture_ = std::move(capture);
        }
        note_session_stopped();
        std::lock_guard<std::mutex> lock(mutex_);
        end_reason_ = reason;
        backend_ = std::string("WASAPI ") + std::string(wasapi_share_mode_name(report.share_mode)) + " "
            + (report.event_driven ? "event" : "poll") + " "
            + std::to_string(report.buffer_milliseconds) + "ms";
        if (stopped.is_error()) {
            diagnostics_ = stopped.error().to_string();
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (overflowed_) {
        const std::size_t dropped = session_.size();
        session_.clear();
        session_.shrink_to_fit();
        drained_ = 0;
        return Result<domain::SampleBuffer>(domain::Error(
            domain::ErrorCode::resource_exhausted,
            "the capture exceeded the 512 MiB recording bound",
            "dropped " + std::to_string(dropped) + " buffered samples"));
    }

    // 3. snapshot: the whole session, never the undrained remainder.
    const std::vector<float> converted = to_16k_mono_locked(0, session_.size());
    session_.clear();
    session_.shrink_to_fit();
    drained_ = 0;
    return Result<domain::SampleBuffer>(make_sample_buffer(converted));
}

Status WindowsAudioCapture::cancel()
{
    std::unique_ptr<WasapiCapture> capture;
    std::unique_ptr<McWasapiSession> native_session;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!recording_) {
            // Idempotent: nothing to stop, nothing to clear, success.
            return Status::success();
        }
        recording_ = false;
        native_session = std::move(mc_session_);
        if (native_session == nullptr) {
            capture = std::move(capture_);
        }
    }

    if (native_session != nullptr) {
        native_session->stop();
        std::lock_guard<std::mutex> lock(mutex_);
        backend_ += " buffers=" + std::to_string(native_buffers_.load(std::memory_order_relaxed))
            + " bytes=" + std::to_string(native_bytes_.load(std::memory_order_relaxed));
        note_session_stopped();
    }

    if (capture != nullptr) {
        (void)capture->stop();
        const WasapiEndReason reason = capture->end_reason();
        capture->destroy();
        std::lock_guard<std::mutex> lock(mutex_);
        end_reason_ = reason;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    session_.clear();
    session_.shrink_to_fit();
    drained_ = 0;
    overflowed_ = false;
    return Status::success();
}

Result<domain::SampleBuffer> WindowsAudioCapture::drain()
{
    std::lock_guard<std::mutex> lock(mutex_);
    const std::vector<float> converted = to_16k_mono_locked(drained_, session_.size());
    // The watermark moves; the samples stay. stop() still sees the whole session.
    drained_ = session_.size();
    return Result<domain::SampleBuffer>(make_sample_buffer(converted));
}

} // namespace voicetyper::platform
