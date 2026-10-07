#pragma once

// Model catalog and download contract.
//
// Evidence: docs/migration/cpp/compatibility-contracts.md §4 "Model catalog";
// .NET reference VoiceTyper.Core/Services/ModelManager.cs,
// VoiceTyper.Core/Models/ModelDownloadProgress.cs and
// VoiceTyper.Core/Services/UpdateService.cs (HTTP defaults).
//
// The catalog below is a *frozen compatibility table*: filenames, base URLs and
// byte sizes are read by existing user installations, so they must match the
// .NET build exactly. A migration that changed a filename would strand every
// already-downloaded model.
//
// Frozen observable contract:
//   * Whisper q8 models and the Silero VAD model come from
//     ggerganov/whisper.cpp and ggml-org/whisper-vad; Parakeet GGUF from
//     mudler/parakeet-cpp-gguf, all under /resolve/main/.
//   * Downloads stream to "<target>.download" and are moved onto the target when
//     finished, so a half-written file is never mistaken for a model.
//   * Progress is reported at most every 120 ms, and once at completion.
//   * A Content-Length header overrides the catalog size for the progress total.
//   * Default HTTP timeout is 30 minutes and the User-Agent is "VoiceTyper/1.0".
//   * Startup removes the exact legacy fp16/q5 filenames, so an old installation
//     does not keep hundreds of megabytes of unusable weights.
//
// Decisions intentionally NOT made here (each needs its own approved policy and
// test, per compatibility-contracts.md §4):
//   * resume of a partial download;
//   * checksum verification of model files;
//   * whether an existing file is accepted purely because it exists, or must be
//     size/format validated. The .NET build accepts it on File.Exists alone.
//   * any size cap on the download buffer.
// This interface therefore exposes ensure()/is_downloaded() without choosing a
// validation policy; a stronger implementation must be a deliberate, recorded
// change, not a side effect of this contract.
//
// Thread affinity: all methods are safe from any thread; downloads run on a
// worker thread and report progress through the ProgressSink. Ownership: the
// returned descriptor is a value; the sink is owned by the caller.

#include "domain/cancellation.hpp"
#include "domain/error.hpp"
#include "domain/settings.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace voicetyper::platform {

using domain::CancellationToken;
using domain::ErrorCode;
using domain::GigaamModelSize;
using domain::ModelSize;
using domain::ParakeetModelSize;
using domain::Result;
using domain::Status;

/// Which family a model belongs to.
enum class ModelKind : std::uint8_t {
    /// ggml Whisper weights, one per ModelSize.
    whisper = 0,
    /// Silero VAD weights used by the VAD recording mode.
    vad = 1,
    /// Parakeet GGUF weights, one per ParakeetModelSize.
    parakeet = 2,
    /// GigaAM-v3 e2e-rnnt GGUF weights, one per GigaamModelSize.
    gigaam = 3,
};

[[nodiscard]] constexpr std::string_view model_kind_name(ModelKind kind) noexcept
{
    switch (kind) {
    case ModelKind::whisper: return "whisper";
    case ModelKind::vad: return "vad";
    case ModelKind::parakeet: return "parakeet";
    case ModelKind::gigaam: return "gigaam";
    }
    return "unknown";
}

/// Base URLs of the model repositories. Frozen for the .NET-shared rows; the
/// GigaAM row is a C++-only extension.
inline constexpr std::string_view kWhisperModelBaseUrl = "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/";
inline constexpr std::string_view kVadModelBaseUrl = "https://huggingface.co/ggml-org/whisper-vad/resolve/main/";
inline constexpr std::string_view kParakeetModelBaseUrl = "https://huggingface.co/mudler/parakeet-cpp-gguf/resolve/main/";
/// GigaAM-v3 e2e-rnnt GGUF quants published by the transcribe.cpp author; the
/// weights inherit the upstream MIT license (see THIRD_PARTY_NOTICES.md).
inline constexpr std::string_view kGigaamModelBaseUrl = "https://huggingface.co/handy-computer/gigaam-v3-e2e-rnnt-gguf/resolve/main/";

/// Default model download timeout (30 minutes). The download also sends the
/// shared User-Agent defined in platform/api/http.hpp.
inline constexpr std::int64_t kModelDownloadTimeoutMinutes = 30;
/// Progress report cadence, milliseconds.
inline constexpr int kModelProgressIntervalMs = 120;
/// Download buffer size, bytes. Frozen to the .NET 128 KiB copy buffer.
inline constexpr std::size_t kModelDownloadBufferBytes = 128 * 1024;
/// Suffix of the in-progress download file.
inline constexpr std::string_view kModelDownloadSuffix = ".download";

/// One entry of the frozen catalog: where a model lives and how big it is.
struct ModelDescriptor {
    ModelKind kind = ModelKind::whisper;
    /// Meaningful for whisper and parakeet; unused for vad and gigaam.
    ModelSize size = ModelSize::small;
    ParakeetModelSize parakeet_size = ParakeetModelSize::q8_0;
    /// Meaningful for gigaam only.
    GigaamModelSize gigaam_size = GigaamModelSize::q8_0;
    /// File name inside the models directory. Frozen.
    std::string file_name;
    /// Full download URL, i.e. base URL + file name. Frozen.
    std::string download_url;
    /// Exact byte size of the published file, for progress and diagnostics.
    std::uint64_t expected_bytes = 0;

    /// True when `path` points at this model, i.e. the file name matches.
    [[nodiscard]] bool matches_path(const std::filesystem::path& path) const;
};

/// Download progress. Mirrors the .NET ModelDownloadProgress record: the same
/// three inputs produce the same fraction and remaining-time estimate, which the
/// differential harness compares.
struct ModelDownloadProgress {
    std::uint64_t bytes_downloaded = 0;
    std::uint64_t total_bytes = 0;
    double bytes_per_second = 0.0;

    /// 0..1, clamped. 0 when total_bytes is unknown.
    [[nodiscard]] double fraction() const noexcept
    {
        if (total_bytes == 0) {
            return 0.0;
        }
        const double value = static_cast<double>(bytes_downloaded) / static_cast<double>(total_bytes);
        return value > 1.0 ? 1.0 : value;
    }

    /// Seconds left, or nullopt when there is not enough information.
    [[nodiscard]] std::optional<double> remaining_seconds() const noexcept
    {
        if (total_bytes == 0 || bytes_downloaded >= total_bytes || bytes_per_second <= 0.0) {
            return std::nullopt;
        }
        return static_cast<double>(total_bytes - bytes_downloaded) / bytes_per_second;
    }
};

/// Receives progress on the download thread. Must be cheap and thread-safe.
using ModelProgressSink = std::function<void(const ModelDownloadProgress&)>;

/// The frozen catalog, as free functions so a UI can list every option without
/// constructing a store.
[[nodiscard]] const std::array<ModelDescriptor, 5>& whisper_catalog();
[[nodiscard]] const std::array<ModelDescriptor, 1>& vad_catalog();
[[nodiscard]] const std::array<ModelDescriptor, 4>& parakeet_catalog();
[[nodiscard]] const std::array<ModelDescriptor, 4>& gigaam_catalog();

[[nodiscard]] Result<ModelDescriptor> find_whisper_model(ModelSize size);
[[nodiscard]] Result<ModelDescriptor> find_vad_model();
[[nodiscard]] Result<ModelDescriptor> find_parakeet_model(ParakeetModelSize size);
[[nodiscard]] Result<ModelDescriptor> find_gigaam_model(GigaamModelSize size);

/// Filenames removed at startup because they predate the q8 weights. Frozen:
/// deleting by exact name only, so an unrelated file is never touched.
[[nodiscard]] const std::vector<std::string>& legacy_model_file_names();

/// Owns the model cache directory and its downloads.
class ModelStore {
public:
    virtual ~ModelStore() = default;

    ModelStore(const ModelStore&) = delete;
    ModelStore& operator=(const ModelStore&) = delete;
    ModelStore(ModelStore&&) = delete;
    ModelStore& operator=(ModelStore&&) = delete;

    /// Root directory of the cache. Must be the same directory the .NET build
    /// used so a side-by-side installation shares downloaded models.
    [[nodiscard]] virtual std::filesystem::path models_directory() const = 0;

    /// Full path of a catalog entry, whether or not it exists.
    [[nodiscard]] virtual std::filesystem::path path_for(const ModelDescriptor& descriptor) const = 0;

    /// Whether the file is present. Existence only; whether that is a sufficient
    /// validity check is an open policy and is not decided here.
    [[nodiscard]] virtual bool is_downloaded(const ModelDescriptor& descriptor) const = 0;

    /// Returns the local path of `descriptor`, downloading it if necessary.
    ///
    /// `progress` may be empty. `cancellation` aborts and leaves no partial file
    /// behind. Failure codes: not_found (the descriptor is not in the catalog),
    /// unavailable (the host is unreachable), timeout, cancelled, io_failure.
    /// A successful result whose file is already present performs no network I/O.
    [[nodiscard]] virtual Result<std::filesystem::path> ensure(
        const ModelDescriptor& descriptor, const ModelProgressSink& progress, const CancellationToken& cancellation) = 0;

    /// Deletes the model file. Returns not_found when it was already absent.
    virtual Status remove(const ModelDescriptor& descriptor) = 0;

    /// Removes the exact legacy fp16/q5 filenames. Idempotent. Must not fail
    /// the whole startup because one legacy file is locked; such a file is
    /// reported and the rest are still attempted.
    virtual Status cleanup_legacy() = 0;

protected:
    ModelStore() = default;
};

} // namespace voicetyper::platform
