#include "asr/engine_host.hpp"

#include "asr/engine_parameters.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const std::string& message)
{
    if (!condition) {
        ++failures;
        std::cerr << "FAIL " << message << '\n';
    }
}

using namespace voicetyper;
using namespace voicetyper::asr;

/// Observation handle that outlives the engine it watches.
///
/// The test must never keep a raw pointer into an engine the host may destroy:
/// that is exactly the use-after-free this contract is meant to prevent, and it
/// also fired under TSan/ASan in the first version of this test.
struct EngineProbe {
    std::atomic<int> warmups{0};
    std::atomic<int> transcribe_calls{0};
    std::atomic<int> destroyed{0};
    std::atomic<std::uint64_t> last_wav_bytes{0};
    std::atomic<int> last_best_of{0};
    std::atomic<int> max_concurrency{0};
    std::atomic<bool> block_transcribe{false};
    std::atomic<bool> release_transcribe{false};
    std::atomic<bool> block_deep_warmup{false};
    std::atomic<bool> release_deep_warmup{false};
    std::atomic<bool> fail_warmup{false};
    std::atomic<bool> fail_transcribe{false};
    std::string tag;
    std::mutex events_mutex;
    std::vector<std::string> events;
};

class FakeTranscriber final : public platform::Transcriber {
public:
    FakeTranscriber(
        domain::TranscriptionEngine engine,
        std::filesystem::path model,
        std::shared_ptr<EngineProbe> probe)
        : engine_(engine), model_(std::move(model)), probe_(std::move(probe))
    {
    }

    ~FakeTranscriber() override
    {
        probe_->destroyed.fetch_add(1);
        std::lock_guard<std::mutex> lock(probe_->events_mutex);
        probe_->events.push_back("free:" + probe_->tag);
    }

    domain::TranscriptionEngine engine() const noexcept override { return engine_; }
    const std::filesystem::path& model_path() const noexcept override { return model_; }
    platform::EngineCapabilities capabilities() const noexcept override
    {
        return engine_ == domain::TranscriptionEngine::whisper ? whisper_capabilities() : parakeet_capabilities();
    }
    bool is_ready() const noexcept override { return ready_.load(std::memory_order_acquire); }

    domain::Status warmup() override
    {
        probe_->warmups.fetch_add(1);
        {
            std::lock_guard<std::mutex> lock(probe_->events_mutex);
            probe_->events.push_back("load:" + probe_->tag);
        }
        if (probe_->fail_warmup.load()) {
            return domain::Status::failure(domain::ErrorCode::model_not_ready, "model rejected");
        }
        ready_.store(true, std::memory_order_release);
        return domain::Status::success();
    }

    domain::Status deep_warmup(const domain::CancellationToken& cancellation) override
    {
        if (probe_->block_deep_warmup.load()) {
            while (!cancellation.is_cancellation_requested() && !probe_->release_deep_warmup.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        return domain::Status::success();
    }

    domain::Result<std::string> transcribe(
        const domain::WavAudio& wav,
        const platform::TranscriptionRequest& request,
        const domain::CancellationToken& cancellation) override
    {
        probe_->transcribe_calls.fetch_add(1);
        const auto concurrent = active_calls_.fetch_add(1) + 1;
        auto previous = probe_->max_concurrency.load();
        while (concurrent > previous && !probe_->max_concurrency.compare_exchange_weak(previous, concurrent)) {
        }
        probe_->last_best_of.store(request.best_of);
        probe_->last_wav_bytes.store(wav.bytes);
        if (probe_->block_transcribe.load()) {
            while (!cancellation.is_cancellation_requested() && !probe_->release_transcribe.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        active_calls_.fetch_sub(1);
        if (cancellation.is_cancellation_requested()) {
            return domain::Result<std::string>::failure(domain::ErrorCode::cancelled, "cancelled in engine");
        }
        if (probe_->fail_transcribe.load()) {
            return domain::Result<std::string>::failure(domain::ErrorCode::io_failure, "engine failure");
        }
        return std::string("текст ") + probe_->tag;
    }

private:
    domain::TranscriptionEngine engine_;
    std::filesystem::path model_;
    std::shared_ptr<EngineProbe> probe_;
    std::atomic<bool> ready_{false};
    std::atomic<int> active_calls_{0};
};

/// Fake registry. It can refuse an engine the way the real one must: no
/// substitution, ever.
class FakeRegistry final : public platform::EngineRegistry {
public:
    platform::EngineAvailability availability(domain::TranscriptionEngine engine) const override
    {
        availability_calls.fetch_add(1);
        platform::EngineAvailability result;
        result.engine = engine;
        if (engine == domain::TranscriptionEngine::parakeet && !library_present) {
            result.available = false;
            result.reason = platform::EngineAvailabilityReason::native_library_missing;
            return result;
        }
        if (engine == domain::TranscriptionEngine::parakeet && abi != platform::kParakeetAbiVersion) {
            result.available = false;
            result.reason = platform::EngineAvailabilityReason::abi_mismatch;
            result.abi_version = abi;
            return result;
        }
        if (model_missing) {
            result.available = false;
            result.reason = platform::EngineAvailabilityReason::model_missing;
            return result;
        }
        result.available = true;
        result.reason = platform::EngineAvailabilityReason::available;
        if (engine == domain::TranscriptionEngine::parakeet) {
            result.abi_version = abi;
        }
        return result;
    }

    domain::Result<std::unique_ptr<platform::Transcriber>> create(
        domain::TranscriptionEngine engine, std::filesystem::path model) override
    {
        create_calls.fetch_add(1);
        last_created_engine = engine;
        if (model_missing) {
            return domain::Result<std::unique_ptr<platform::Transcriber>>::failure(
                domain::ErrorCode::not_found, "model file is absent");
        }
        if (create_fails) {
            return domain::Result<std::unique_ptr<platform::Transcriber>>::failure(
                create_error_code, create_error_message);
        }
        auto probe = std::make_shared<EngineProbe>();
        probe->tag = "engine" + std::to_string(++tags);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            probes.push_back(probe);
        }
        return std::unique_ptr<platform::Transcriber>(new FakeTranscriber(engine, model, std::move(probe)));
    }

    domain::Status set_model_path(domain::TranscriptionEngine, std::filesystem::path) override
    {
        return domain::Status::success();
    }
    std::optional<std::filesystem::path> model_path(domain::TranscriptionEngine) const override
    {
        return std::nullopt;
    }

    std::shared_ptr<EngineProbe> probe(std::size_t index) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return index < probes.size() ? probes[index] : nullptr;
    }

    bool library_present = true;
    int abi = platform::kParakeetAbiVersion;
    bool model_missing = false;
    bool create_fails = false;
    domain::ErrorCode create_error_code = domain::ErrorCode::model_not_ready;
    std::string create_error_message = "load failed";
    mutable std::atomic<int> availability_calls{0};
    std::atomic<int> create_calls{0};
    domain::TranscriptionEngine last_created_engine = domain::TranscriptionEngine::whisper;
    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<EngineProbe>> probes;
    int tags = 0;
};

domain::SampleBuffer short_audio()
{
    domain::SampleBuffer buffer(16000);
    const std::vector<float> samples(4000, 0.25f);
    static_cast<void>(buffer.append(samples));
    return buffer;
}

void check_transcribe_before_load()
{
    FakeRegistry registry;
    EngineHost host(registry);
    check(!host.is_ready_for_recording(), "a host without a selection is not ready");
    check(host.state().readiness == EngineReadiness::idle, "the initial readiness is idle");

    const auto result = host.transcribe(short_audio(), domain::SessionOptions{}, {});
    check(result.is_error() && result.code() == domain::ErrorCode::model_not_ready,
        "transcribe before a model is loaded is model_not_ready");
    check(registry.create_calls.load() == 0, "transcribe never creates an engine lazily");
}

void check_load_and_transcribe()
{
    FakeRegistry registry;
    EngineHost host(registry);
    host.select(domain::TranscriptionEngine::whisper, "/models/ggml-small-q8_0.bin");
    check(host.state().readiness == EngineReadiness::loading, "select publishes loading immediately");
    check(host.wait_until_settled(std::chrono::seconds(5)), "the background load settles");
    check(host.state().readiness == EngineReadiness::ready, "a successful load publishes ready");
    check(host.is_ready_for_recording(), "a ready engine accepts dictation");
    check(registry.create_calls.load() == 1, "exactly one engine was created");
    check(registry.last_created_engine == domain::TranscriptionEngine::whisper, "the requested engine was created");

    const auto probe = registry.probe(0);
    check(probe != nullptr && probe->warmups.load() == 1, "warmup ran exactly once");

    const auto result = host.transcribe(short_audio(), domain::SessionOptions{}, {});
    check(result.is_ok() && result.value() == "текст engine1", "a ready engine returns its transcript");
    check(probe->last_wav_bytes.load() == 44 + 4000 * 2,
        "the engine received a canonical WAV of the captured audio");
    check(probe->last_best_of.load() == 3, "the final result asks for 3 candidates");
    static_cast<void>(host.shutdown(std::chrono::seconds(2)));
    check(probe->destroyed.load() == 1, "shutdown frees the engine exactly once");
}

void check_unavailable_engine_never_falls_back()
{
    FakeRegistry registry;
    registry.library_present = false;
    EngineHost host(registry);
    host.select(domain::TranscriptionEngine::parakeet, "/models/tdt-0.6b-v3-q8_0.gguf");
    check(host.wait_until_settled(std::chrono::seconds(5)), "the unavailable selection settles");
    check(host.state().readiness == EngineReadiness::unavailable, "a missing DLL is unavailable");
    check(host.state().reason == platform::EngineAvailabilityReason::native_library_missing,
        "the reason names the missing library");
    check(registry.create_calls.load() == 0, "no engine is created for an unavailable machine");
    const auto result = host.transcribe(short_audio(), domain::SessionOptions{}, {});
    check(result.is_error() && result.code() == domain::ErrorCode::engine_unavailable,
        "transcribe reports engine_unavailable");
    static_cast<void>(host.shutdown(std::chrono::seconds(2)));
}

void check_abi_mismatch()
{
    FakeRegistry registry;
    registry.abi = 5;
    EngineHost host(registry);
    host.select(domain::TranscriptionEngine::parakeet, "/models/tdt-0.6b-v3-q8_0.gguf");
    check(host.wait_until_settled(std::chrono::seconds(5)), "the ABI-mismatched selection settles");
    check(host.state().readiness == EngineReadiness::unavailable, "an ABI mismatch is unavailable");
    check(host.state().reason == platform::EngineAvailabilityReason::abi_mismatch, "the reason is abi_mismatch");
    check(host.state().abi_version.value_or(-1) == 5, "the observed ABI is reported for diagnostics");
    check(registry.create_calls.load() == 0, "an ABI mismatch creates nothing");
    static_cast<void>(host.shutdown(std::chrono::seconds(2)));
}

void check_missing_model()
{
    FakeRegistry registry;
    registry.model_missing = true;
    EngineHost host(registry);
    host.select(domain::TranscriptionEngine::whisper, "/models/gone.bin");
    check(host.wait_until_settled(std::chrono::seconds(5)), "the missing-model selection settles");
    check(host.state().readiness == EngineReadiness::model_missing, "a missing model is model_missing");
    const auto result = host.transcribe(short_audio(), domain::SessionOptions{}, {});
    check(result.is_error() && result.code() == domain::ErrorCode::engine_unavailable,
        "a missing model is reported as engine_unavailable, never as a different engine");
    static_cast<void>(host.shutdown(std::chrono::seconds(2)));
}

void check_availability_is_side_effect_free()
{
    FakeRegistry registry;
    const auto first = registry.availability(domain::TranscriptionEngine::parakeet);
    const auto second = registry.availability(domain::TranscriptionEngine::parakeet);
    check(first.available == second.available && first.reason == second.reason,
        "repeated availability queries return the same snapshot");
    check(registry.create_calls.load() == 0, "availability never creates an engine");
}

void check_superseded_load_never_publishes()
{
    FakeRegistry registry;
    EngineHost host(registry);
    host.select(domain::TranscriptionEngine::whisper, "/models/first.bin");
    const auto first_epoch = host.epoch();
    host.select(domain::TranscriptionEngine::whisper, "/models/second.bin");
    check(host.epoch() > first_epoch, "a new selection bumps the epoch");
    check(host.wait_until_settled(std::chrono::seconds(5)), "the second selection settles");
    check(host.state().model_path == "/models/second.bin", "the newest model path is the published one");
    check(host.state().readiness == EngineReadiness::ready, "the newest load publishes ready");
    static_cast<void>(host.shutdown(std::chrono::seconds(2)));
}

void check_cancellation()
{
    FakeRegistry registry;
    EngineHost host(registry);
    host.select(domain::TranscriptionEngine::whisper, "/models/ggml-small-q8_0.bin");
    check(host.wait_until_settled(std::chrono::seconds(5)), "the engine is ready for the cancellation case");
    const auto probe = registry.probe(0);

    domain::CancellationSource pre;
    pre.request_cancellation();
    const auto before = host.transcribe(short_audio(), domain::SessionOptions{}, pre.token());
    check(before.is_error() && before.code() == domain::ErrorCode::cancelled,
        "a pre-cancelled token stops before the engine is touched");
    check(probe->transcribe_calls.load() == 0, "a pre-cancelled call never enters the engine");

    probe->block_transcribe = true;
    domain::CancellationSource during;
    std::string outcome;
    std::thread worker([&] {
        const auto result = host.transcribe(short_audio(), domain::SessionOptions{}, during.token());
        outcome = result.is_error() ? std::string(result.error().message()) : result.value();
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    during.request_cancellation();
    probe->release_transcribe = true;
    worker.join();

    check(probe->transcribe_calls.load() == 1, "the engine was called exactly once");
    check(outcome.find("cancelled") != std::string::npos, "a late result is discarded as cancelled");
    check(probe->destroyed.load() == 0, "the engine is not freed while the host is alive");
    static_cast<void>(host.shutdown(std::chrono::seconds(2)));
    check(probe->destroyed.load() == 1, "shutdown frees the engine exactly once");
}

void check_observation_never_blocks()
{
    FakeRegistry registry;
    EngineHost host(registry);
    host.select(domain::TranscriptionEngine::whisper, "/models/ggml-small-q8_0.bin");
    check(host.wait_until_settled(std::chrono::seconds(5)), "the engine is ready");
    const auto probe = registry.probe(0);

    probe->block_transcribe = true;
    const auto started = std::chrono::steady_clock::now();
    std::thread worker([&] {
        static_cast<void>(host.transcribe(short_audio(), domain::SessionOptions{}, {}));
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const bool ready_now = host.is_ready_for_recording();
    const auto snapshot = host.state();
    const auto elapsed = std::chrono::steady_clock::now() - started;
    check(ready_now && snapshot.readiness == EngineReadiness::ready, "readiness is observable while inference runs");
    check(elapsed < std::chrono::seconds(1), "observation does not block on the inference mutex");
    probe->release_transcribe = true;
    worker.join();
    static_cast<void>(host.shutdown(std::chrono::seconds(2)));
}

void check_shutdown_with_busy_engine()
{
    FakeRegistry registry;
    EngineHost host(registry);
    host.select(domain::TranscriptionEngine::whisper, "/models/ggml-small-q8_0.bin");
    check(host.wait_until_settled(std::chrono::seconds(5)), "the engine is ready");
    const auto probe = registry.probe(0);

    probe->block_transcribe = true;
    std::thread worker([&] {
        static_cast<void>(host.transcribe(short_audio(), domain::SessionOptions{}, {}));
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // A deadline that expires while a non-interruptible engine is still running
    // must report the timeout instead of freeing a live context.
    const auto early = host.shutdown(std::chrono::milliseconds(10));
    check(early.is_error() && early.code() == domain::ErrorCode::timeout,
        "shutdown reports a timeout instead of freeing a running context");
    check(probe->destroyed.load() == 0, "a running engine is never freed");
    probe->release_transcribe = true;
    worker.join();
    static_cast<void>(host.shutdown(std::chrono::seconds(2)));
    check(probe->destroyed.load() == 1, "the engine is released once the inference finished");
}

void check_failed_load_reports_error()
{
    FakeRegistry registry;
    registry.create_fails = true;
    registry.create_error_code = domain::ErrorCode::model_not_ready;
    registry.create_error_message = "model rejected by loader";
    EngineHost host(registry);
    host.select(domain::TranscriptionEngine::parakeet, "/models/tdt-0.6b-v3-q8_0.gguf");
    check(host.wait_until_settled(std::chrono::seconds(5)), "the failed load settles");
    check(host.state().readiness == EngineReadiness::failed, "a load failure is a failed state");
    check(host.state().last_error == "model rejected by loader", "the failure detail is preserved");
    check(!host.is_ready_for_recording(), "a failed engine does not accept dictation");
    static_cast<void>(host.shutdown(std::chrono::seconds(2)));
}

} // namespace

int main()
{
    check_transcribe_before_load();
    check_load_and_transcribe();
    check_unavailable_engine_never_falls_back();
    check_abi_mismatch();
    check_missing_model();
    check_availability_is_side_effect_free();
    check_superseded_load_never_publishes();
    check_cancellation();
    check_observation_never_blocks();
    check_shutdown_with_busy_engine();
    check_failed_load_reports_error();

    if (failures != 0) {
        std::cerr << "asr-lifecycle-contract: " << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "asr-lifecycle-contract: OK\n";
    return 0;
}
