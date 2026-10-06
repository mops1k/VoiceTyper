#include "domain/vad.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace voicetyper::domain {

namespace {

/// RMS of one frame. Cheap, allocation-free, and enough to separate speech from
/// a quiet room; it is not a substitute for a neural model and does not pretend
/// to be.
double frame_rms(const std::vector<float>& samples, std::size_t begin, std::size_t end)
{
    if (end <= begin) {
        return 0.0;
    }
    double sum = 0.0;
    for (std::size_t i = begin; i < end; ++i) {
        sum += static_cast<double>(samples[i]) * static_cast<double>(samples[i]);
    }
    return std::sqrt(sum / static_cast<double>(end - begin));
}

} // namespace

EnergySpeechSegmenter::EnergySpeechSegmenter(
    double sample_rate, double frame_seconds, double speech_over_noise_ratio)
    : sample_rate_(static_cast<std::size_t>(sample_rate > 0.0 ? sample_rate : 16000.0))
    , frame_samples_(static_cast<std::size_t>(
          (sample_rate > 0.0 ? sample_rate : 16000.0) * (frame_seconds > 0.0 ? frame_seconds : 0.03)))
    , speech_over_noise_ratio_(speech_over_noise_ratio > 1.0 ? speech_over_noise_ratio : 3.0)
{
    if (frame_samples_ == 0) {
        frame_samples_ = 1;
    }
}

void EnergySpeechSegmenter::reset()
{
    noise_floor_ = 0.0;
    floor_initialised_ = false;
    pending_speech_start_seconds_ = 0.0;
    in_speech_ = false;
    fed_seconds_ = 0.0;
}

std::vector<SpeechSegment> EnergySpeechSegmenter::detect_speech_no_reset(
    const std::vector<float>& samples)
{
    std::vector<SpeechSegment> segments;
    if (samples.empty()) {
        return segments;
    }

    const auto frames = samples.size() / frame_samples_;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const auto begin = frame * frame_samples_;
        const auto rms = frame_rms(samples, begin, begin + frame_samples_);
        const double frame_start = static_cast<double>(begin) / static_cast<double>(sample_rate_);

        if (!floor_initialised_) {
            // The first frame defines the floor; a session that starts with
            // speech would then have a high floor, which is why the floor only
            // ever moves downwards fast and upwards slowly.
            noise_floor_ = rms;
            floor_initialised_ = true;
        } else if (rms < noise_floor_) {
            noise_floor_ = noise_floor_ * 0.9 + rms * 0.1;
        }

        const bool speech = rms > noise_floor_ * speech_over_noise_ratio_
            || rms > 0.02; // absolute escape so a digital-silent floor is not latched
        if (speech) {
            if (!in_speech_) {
                in_speech_ = true;
                pending_speech_start_seconds_ = frame_start;
            }
        } else if (in_speech_) {
            in_speech_ = false;
            SpeechSegment segment;
            segment.start_seconds = pending_speech_start_seconds_;
            segment.end_seconds = frame_start;
            segments.push_back(segment);
        }
    }
    fed_seconds_ += static_cast<double>(samples.size()) / static_cast<double>(sample_rate_);

    // A segment still open at the end of the chunk is reported up to the last
    // frame boundary, because the caller may be a streaming loop: reporting an
    // unbounded end would claim audio that has not been fed yet.
    if (in_speech_) {
        const auto consumed = static_cast<double>(frames * frame_samples_) / static_cast<double>(sample_rate_);
        if (consumed > pending_speech_start_seconds_) {
            SpeechSegment segment;
            segment.start_seconds = pending_speech_start_seconds_;
            segment.end_seconds = consumed;
            segments.push_back(segment);
        }
    }
    return segments;
}

CallbackSpeechSegmenter::CallbackSpeechSegmenter(Callback callback)
    : callback_(std::move(callback))
{
}

std::vector<SpeechSegment> CallbackSpeechSegmenter::detect_speech_no_reset(
    const std::vector<float>& samples)
{
    if (!callback_) {
        return {};
    }
    return callback_(samples);
}

void CallbackSpeechSegmenter::reset()
{
}

SilenceAutoStopDetector::SilenceAutoStopDetector(
    SpeechSegmenter& segmenter,
    double silence_threshold_seconds,
    std::size_t sample_rate)
    : segmenter_(segmenter)
    , silence_threshold_seconds_(std::max(0.0, silence_threshold_seconds))
    , hold_seconds_(std::min(0.8, std::max(0.3, silence_threshold_seconds / 2.0)))
    , sample_rate_(sample_rate == 0 ? 16000 : sample_rate)
{
}

void SilenceAutoStopDetector::reset() noexcept
{
    segmenter_.reset();
    total_fed_seconds_ = 0.0;
    last_speech_end_seconds_ = 0.0;
    speech_detected_ = false;
    noise_floor_ = 1e-5;
    consecutive_inactive_chunks_ = 0;
}

VadStopDecision SilenceAutoStopDetector::process(const std::vector<float>& samples)
{
    if (samples.empty()) {
        return {};
    }

    const auto chunk_duration = static_cast<double>(samples.size()) / static_cast<double>(sample_rate_);
    const auto chunk_end = total_fed_seconds_ + chunk_duration;
    const auto segments = segmenter_.detect_speech_no_reset(samples);
    bool speech_near_end = false;
    for (const auto& segment : segments) {
        const auto end_seconds = segment.end_seconds;
        const auto absolute_end = end_seconds <= chunk_duration
            ? total_fed_seconds_ + end_seconds
            : end_seconds;
        speech_detected_ = true;
        if (absolute_end > last_speech_end_seconds_) {
            last_speech_end_seconds_ = absolute_end;
        }
        const auto gap_from_end = chunk_end - absolute_end;
        if (gap_from_end >= 0.0 && gap_from_end <= hold_seconds_) {
            speech_near_end = true;
        }
    }

    double sum = 0.0;
    for (const auto sample : samples) {
        const auto value = static_cast<double>(sample);
        sum += value * value;
    }
    const auto energy = sum / static_cast<double>(samples.size());
    const auto energy_active = energy > noise_floor_ * 3.0;
    if (energy < noise_floor_) {
        noise_floor_ = noise_floor_ * 0.7 + energy * 0.3;
    } else if (!energy_active) {
        noise_floor_ = std::max(1e-8, noise_floor_ * 1.001);
    }

    const auto active_chunk = energy_active || speech_near_end;
    if (energy_active || speech_near_end) {
        speech_detected_ = true;
        last_speech_end_seconds_ = std::max(last_speech_end_seconds_, chunk_end);
    }
    total_fed_seconds_ = chunk_end;

    if (!speech_detected_) {
        if (total_fed_seconds_ >= 5.0) {
            return VadStopDecision{true, VadStopReason::no_speech_idle};
        }
        return {};
    }

    consecutive_inactive_chunks_ = active_chunk ? 0 : consecutive_inactive_chunks_ + 1;
    const auto silence_seconds = total_fed_seconds_ - last_speech_end_seconds_;
    if (consecutive_inactive_chunks_ >= 1 && silence_seconds >= silence_threshold_seconds_) {
        return VadStopDecision{true, VadStopReason::trailing_silence};
    }
    return {};
}

} // namespace voicetyper::domain
