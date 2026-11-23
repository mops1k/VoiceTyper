#pragma once

// A RecordingPort whose capture object can be replaced between dictations.
//
// Why it exists: the capture route (which endpoint, which backend) is now decided
// by capture_path_resolver.hpp at startup and again when the device list changes.
// The recording machine, the logged decorator and the settings test all hold a
// reference to ONE port for the whole run, so the object they hold must be able to
// switch the underlying WindowsAudioCapture: recreating the port would leave those
// references dangling. A swap never touches an open session - the next start()
// builds the new capture from the current factory.
//
// Duck-typed like the resolver: the underlying type only has to provide the
// application-facing capture surface, so a fake stands in for it in the tests.

#include "audio/audio_format.hpp"
#include "audio/audio_wav.hpp"
#include "domain/cancellation.hpp"
#include "domain/error.hpp"
#include "domain/recording_state_machine.hpp"

#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace voicetyper::app {

template <typename Capture>
class SwitchingCapture final : public domain::RecordingPort {
public:
    using Factory = std::function<std::unique_ptr<Capture>()>;

    explicit SwitchingCapture(Factory factory)
        : factory_(std::move(factory))
    {
    }

    /// Points the port at another capture definition. An open session is left
    /// alone: the switch takes effect at the next start(), so a resolution that
    /// finishes mid-dictation can never cut the user's recording.
    void reconfigure(Factory factory)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        factory_ = std::move(factory);
        if (recording_) {
            // The open session keeps its capture; the new factory is applied by the
            // next start(), so a resolution that finishes mid-dictation can never cut
            // the user's recording.
            pending_swap_ = true;
        } else {
            capture_.reset();
            pending_swap_ = false;
        }
    }

    /// Drops the idle capture so its endpoint is released before the resolver probes
    /// it. A held IAudioClient keeps an exclusive endpoint allocated for this process,
    /// and probing it from the same process then answers AUDCLNT_E_DEVICE_IN_USE
    /// (0x8889000A) - measured 2026-11-23, when a re-resolution rejected the very
    /// microphone the running port had warmed up. An open session is left alone.
    void release_idle_capture()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (recording_) {
            return;
        }
        capture_.reset();
        pending_swap_ = false;
    }

    /// True when the port wants a fresh capture at the next start() (the factory
    /// changed while idle, or while a session that has since ended was open).
    [[nodiscard]] bool needs_new_capture() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return capture_ == nullptr || pending_swap_;
    }

    domain::Status start(const domain::CancellationToken& cancellation) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (recording_) {
            return domain::Status::failure(
                domain::ErrorCode::invalid_state, "a recording session is already running");
        }
        if (pending_swap_) {
            capture_.reset();
            pending_swap_ = false;
        }
        if (capture_ == nullptr) {
            if (!factory_) {
                return domain::Status::failure(
                    domain::ErrorCode::invalid_state, "no capture route is configured");
            }
            capture_ = factory_();
            if (capture_ == nullptr) {
                return domain::Status::failure(
                    domain::ErrorCode::unavailable, "the capture route produced no capture object");
            }
        }
        const auto status = capture_->start(cancellation);
        if (status.is_ok()) {
            recording_ = true;
        }
        return status;
    }

    domain::Result<audio::SampleBuffer> stop() override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (capture_ == nullptr) {
            return domain::Result<audio::SampleBuffer>(domain::Error(
                domain::ErrorCode::invalid_state, "no capture session was started"));
        }
        recording_ = false;
        return capture_->stop();
    }

    domain::Status cancel() override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        recording_ = false;
        if (capture_ == nullptr) {
            return domain::Status::success();
        }
        return capture_->cancel();
    }

    domain::Result<audio::SampleBuffer> drain() override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (capture_ == nullptr) {
            return domain::Result<audio::SampleBuffer>(domain::Error(
                domain::ErrorCode::invalid_state, "no capture session was started"));
        }
        return capture_->drain();
    }

    [[nodiscard]] bool is_recording() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return recording_;
    }

    // --- the application-facing surface of the capture backends --------------
    [[nodiscard]] double live_peak() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return capture_ == nullptr ? 0.0 : capture_->live_peak();
    }

    [[nodiscard]] std::string backend_description() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return capture_ == nullptr ? std::string("no capture route") : capture_->backend_description();
    }

    [[nodiscard]] std::string device_id() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return capture_ == nullptr ? std::string() : capture_->device_id();
    }

private:
    mutable std::mutex mutex_;
    Factory factory_;
    std::unique_ptr<Capture> capture_;
    bool recording_ = false;
    bool pending_swap_ = false;
};

} // namespace voicetyper::app
