#include "asr/engine_host.hpp"

#include "asr/engine_parameters.hpp"
#include "domain/audio_wav.hpp"

#include <algorithm>
#include <vector>

namespace voicetyper::asr {

// ---------------------------------------------------------------------------
// EngineExecutor
// ---------------------------------------------------------------------------

EngineExecutor::EngineExecutor()
{
    std::lock_guard<std::mutex> lock(mutex_);
    running_ = true;
    thread_ = std::thread([this] {
        for (;;) {
            Task task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                condition_.wait(lock, [this] { return !running_ || !queue_.empty(); });
                if (!running_ && queue_.empty()) {
                    return;
                }
                // Newest first: a model switch supersedes the queued load it
                // replaces, and the stale one is dropped instead of run.
                task = std::move(queue_.back().second);
                queue_.pop_back();
            }
            if (task) {
                task();
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
            }
            condition_.notify_all();
        }
    });
}

EngineExecutor::~EngineExecutor()
{
    shutdown();
}

void EngineExecutor::post(std::uint64_t epoch, Task task)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_ || !task) {
            return;
        }
        if (epoch < newest_epoch_) {
            return; // superseded before it was even queued
        }
        newest_epoch_ = epoch;
        queue_.erase(
            std::remove_if(queue_.begin(), queue_.end(),
                [epoch](const auto& entry) { return entry.first < epoch; }),
            queue_.end());
        queue_.emplace_back(epoch, std::move(task));
    }
    condition_.notify_all();
}

void EngineExecutor::drain()
{
    for (;;) {
        Task task;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (queue_.empty()) {
                return;
            }
            task = std::move(queue_.back().second);
            queue_.pop_back();
        }
        if (task) {
            task();
        }
    }
}

bool EngineExecutor::wait_idle(std::chrono::milliseconds timeout)
{
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(lock, timeout, [this] { return queue_.empty(); });
}

void EngineExecutor::shutdown()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_ && !thread_.joinable()) {
            return;
        }
        running_ = false;
        queue_.clear();
    }
    condition_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
}

std::size_t EngineExecutor::pending() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

std::uint64_t EngineExecutor::newest_epoch() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return newest_epoch_;
}

// ---------------------------------------------------------------------------
// EngineHost
// ---------------------------------------------------------------------------

std::string_view engine_readiness_name(EngineReadiness readiness) noexcept
{
    switch (readiness) {
    case EngineReadiness::idle: return "idle";
    case EngineReadiness::unavailable: return "unavailable";
    case EngineReadiness::model_missing: return "model_missing";
    case EngineReadiness::loading: return "loading";
    case EngineReadiness::ready: return "ready";
    case EngineReadiness::warming: return "warming";
    case EngineReadiness::failed: return "failed";
    case EngineReadiness::shutting_down: return "shutting_down";
    }
    return "unknown";
}

EngineHost::EngineHost(platform::EngineRegistry& registry)
    : registry_(registry)
{
}

EngineHost::~EngineHost()
{
    static_cast<void>(shutdown(std::chrono::milliseconds(2000)));
}

std::uint64_t EngineHost::epoch() const noexcept
{
    return epoch_.load(std::memory_order_acquire);
}

void EngineHost::select(domain::TranscriptionEngine engine, std::filesystem::path model_path)
{
    const auto next_epoch = epoch_.fetch_add(1, std::memory_order_acq_rel) + 1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutting_down_) {
            return;
        }
        EngineStateSnapshot snapshot;
        snapshot.readiness = EngineReadiness::loading;
        snapshot.engine = engine;
        snapshot.model_path = model_path.string();
        snapshot.reason = platform::EngineAvailabilityReason::available;
        state_ = snapshot;
    }
    settled_.notify_all();
    executor_.post(next_epoch, [this, next_epoch, engine, model_path = std::move(model_path)] {
        run_load(next_epoch, engine, model_path);
    });
}

void EngineHost::run_load(std::uint64_t load_epoch, domain::TranscriptionEngine engine, std::filesystem::path model_path)
{
    const auto availability = registry_.availability(engine);
    if (!availability.available) {
        EngineStateSnapshot snapshot;
        snapshot.readiness = availability.reason == platform::EngineAvailabilityReason::model_missing
            ? EngineReadiness::model_missing
            : EngineReadiness::unavailable;
        snapshot.engine = engine;
        snapshot.model_path = model_path.string();
        snapshot.reason = availability.reason;
        snapshot.abi_version = availability.abi_version;
        snapshot.last_error = std::string(engine_availability_reason_name(availability.reason));
        publish(load_epoch, std::move(snapshot), nullptr);
        return;
    }

    auto created = registry_.create(engine, model_path);
    if (created.is_error()) {
        EngineStateSnapshot snapshot;
        snapshot.readiness = created.code() == ErrorCode::not_found
            ? EngineReadiness::model_missing
            : (created.code() == ErrorCode::engine_unavailable
                    ? EngineReadiness::unavailable
                    : EngineReadiness::failed);
        snapshot.engine = engine;
        snapshot.model_path = model_path.string();
        snapshot.reason = availability.reason;
        snapshot.last_error = created.error().message();
        publish(load_epoch, std::move(snapshot), nullptr);
        return;
    }

    // From here the engine is owned by shared_ptr, so a newer select() can never
    // free the native context while this load is still warming it up.
    auto engine_ptr = std::shared_ptr<platform::Transcriber>(std::move(created.value()));
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutting_down_ || load_epoch != epoch_.load(std::memory_order_acquire)) {
            return; // superseded: the engine dies here, and publishes nothing
        }
        state_.readiness = EngineReadiness::warming;
    }
    settled_.notify_all();

    const auto warmed = engine_ptr->warmup();
    if (warmed.is_error()) {
        EngineStateSnapshot snapshot;
        snapshot.readiness = EngineReadiness::failed;
        snapshot.engine = engine;
        snapshot.model_path = model_path.string();
        snapshot.reason = availability.reason;
        snapshot.abi_version = availability.abi_version;
        snapshot.last_error = warmed.error().message();
        publish(load_epoch, std::move(snapshot), nullptr);
        return;
    }
    const domain::CancellationToken none;
    const auto deep = engine_ptr->deep_warmup(none);
    if (deep.is_error()) {
        EngineStateSnapshot snapshot;
        snapshot.readiness = EngineReadiness::failed;
        snapshot.engine = engine;
        snapshot.model_path = model_path.string();
        snapshot.reason = availability.reason;
        snapshot.abi_version = availability.abi_version;
        snapshot.last_error = deep.error().message();
        publish(load_epoch, std::move(snapshot), nullptr);
        return;
    }

    EngineStateSnapshot snapshot;
    snapshot.readiness = EngineReadiness::ready;
    snapshot.engine = engine;
    snapshot.model_path = model_path.string();
    snapshot.reason = platform::EngineAvailabilityReason::available;
    snapshot.abi_version = availability.abi_version;
    publish(load_epoch, std::move(snapshot), std::move(engine_ptr));
}

void EngineHost::publish(
    std::uint64_t publish_epoch, EngineStateSnapshot snapshot, std::shared_ptr<platform::Transcriber> engine)
{
    std::vector<std::shared_ptr<platform::Transcriber>> to_retire;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (publish_epoch != epoch_.load(std::memory_order_acquire) || shutting_down_) {
            return; // a newer selection won: publish nothing, drop the engine here
        }
        state_ = std::move(snapshot);
        if (engine) {
            if (active_) {
                retired_.push_back(active_); // freed only when nothing is in flight
            }
            active_ = std::move(engine);
        }
        // Engines that were retired while nothing was running can go now.
        to_retire.swap(retired_);
        if (in_flight_.load(std::memory_order_acquire) != 0) {
            retired_ = std::move(to_retire);
            to_retire.clear();
        }
    }
    to_retire.clear();
    settled_.notify_all();
}

void EngineHost::retire_locked(std::shared_ptr<platform::Transcriber> engine)
{
    static_cast<void>(engine);
}

EngineStateSnapshot EngineHost::state() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

bool EngineHost::is_ready_for_recording() const noexcept
{
    const auto current = state().readiness;
    return current == EngineReadiness::ready || current == EngineReadiness::warming;
}

bool EngineHost::wait_until_settled(std::chrono::milliseconds timeout)
{
    std::unique_lock<std::mutex> lock(mutex_);
    return settled_.wait_for(lock, timeout, [this] {
            return state_.readiness == EngineReadiness::ready
                || state_.readiness == EngineReadiness::failed
                || state_.readiness == EngineReadiness::unavailable
                || state_.readiness == EngineReadiness::model_missing;
        });
}

domain::Result<std::string> EngineHost::transcribe(
    const domain::SampleBuffer& audio,
    const domain::SessionOptions& options,
    const domain::CancellationToken& cancellation)
{
    const auto cancelled = check_cancelled(cancellation);
    if (cancelled.is_error()) {
        return domain::Result<std::string>::failure(cancelled.code(), cancelled.message());
    }

    std::shared_ptr<platform::Transcriber> engine;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutting_down_) {
            return domain::Result<std::string>::failure(
                ErrorCode::engine_unavailable, "the engine host is shutting down");
        }
        switch (state_.readiness) {
        case EngineReadiness::unavailable:
            return domain::Result<std::string>::failure(
                ErrorCode::engine_unavailable, "the selected engine cannot run on this machine");
        case EngineReadiness::model_missing:
            return domain::Result<std::string>::failure(
                ErrorCode::engine_unavailable, "no model is selected");
        case EngineReadiness::loading:
        case EngineReadiness::failed:
        case EngineReadiness::idle:
            return domain::Result<std::string>::failure(
                ErrorCode::model_not_ready, "the model is not loaded");
        case EngineReadiness::ready:
        case EngineReadiness::warming:
        case EngineReadiness::shutting_down:
            break;
        }
        engine = active_;
    }
    if (!engine) {
        return domain::Result<std::string>::failure(
            ErrorCode::model_not_ready, "the model is not loaded");
    }

    // Capture audio -> the canonical WAV the frozen transcriber contract takes.
    std::string wav_bytes;
    const auto written = domain::write_wav_pcm16(audio.samples(), wav_bytes);
    if (written.is_error()) {
        return domain::Result<std::string>::failure(written.code(), written.message());
    }
    std::vector<std::byte> wav_data(wav_bytes.size());
    std::transform(wav_bytes.begin(), wav_bytes.end(), wav_data.begin(), [](char value) {
        return static_cast<std::byte>(static_cast<unsigned char>(value));
    });

    platform::TranscriptionRequest request;
    request.language = options.language;
    request.prompt = options.prompt;
    request.temperature = options.temperature;
    request.condition_on_previous_text = options.condition_on_previous_text;
    request.best_of = options.best_of;

    in_flight_.fetch_add(1, std::memory_order_acq_rel);
    auto result = engine->transcribe(domain::WavAudio(std::move(wav_data)), request, cancellation);
    in_flight_.fetch_sub(1, std::memory_order_acq_rel);

    if (cancellation.is_cancellation_requested()) {
        // A late result is discarded: the session that requested it is gone.
        return domain::Result<std::string>::failure(ErrorCode::cancelled, "transcription cancelled");
    }
    if (result.is_error()) {
        return domain::Result<std::string>::failure(result.code(), result.error().message());
    }
    return result;
}

domain::Status EngineHost::shutdown(std::chrono::milliseconds deadline)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!shutting_down_) {
            shutting_down_ = true;
            state_.readiness = EngineReadiness::shutting_down;
        }
        // A repeated shutdown() must still finish releasing the engines once the
        // in-flight inference ends, so it does not return early here.
    }
    settled_.notify_all();
    executor_.shutdown();

    const auto deadline_point = std::chrono::steady_clock::now() + deadline;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        while (in_flight_.load(std::memory_order_acquire) != 0) {
            if (settled_.wait_until(lock, deadline_point) == std::cv_status::timeout) {
                // A non-interruptible engine is still running. Freeing its context
                // now would be a use-after-free, so it is intentionally leaked and
                // released by process exit instead.
                return domain::Status::failure(
                    ErrorCode::timeout, "engine still running at shutdown; context left to process exit");
            }
        }
        retired_.clear();
        active_.reset();
    }
    return domain::Status::success();
}

} // namespace voicetyper::asr
