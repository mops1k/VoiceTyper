#pragma once

// Model downloader.
//
// The .NET build (VoiceTyper.Core/Services/ModelManager.cs) fetched the model catalog from two
// Hugging Face repositories into the models directory and reported progress as a fraction plus
// an estimate of the remaining time (VoiceTyper.Core/Models/ModelDownloadProgress.cs). This is
// that flow, written against the same platform::HttpClient seam the update service uses, so it
// can be tested without a network.
//
// One deliberate difference from the .NET code: the bytes are streamed into "<name>.part" and
// the file is renamed only after a complete transfer. Writing straight into the target file
// leaves a half-written .bin behind when a connection drops, and everything else - the model
// list, the recogniser - treats a file of the right name as a ready model.

#include "domain/settings.hpp"
#include "platform/api/http.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace voicetyper::core::support {

/// The repositories the .NET catalog downloaded from, plus the C++-only GigaAM
/// repository (handy-computer/gigaam-v3-e2e-rnnt-gguf, MIT weights).
enum class ModelEngine { whisper, parakeet, gigaam };

/// One transfer may take a long time: the largest Parakeet quant is about 0.9 GB, and a slow
/// connection is normal rather than an error.
inline constexpr std::chrono::seconds kModelDownloadTimeout{7200};

/// Progress of one download, mirroring the .NET record of the same name.
struct ModelDownloadProgress {
    std::uint64_t downloaded = 0;
    /// Zero when the server did not report a length.
    std::uint64_t total = 0;
    double bytes_per_second = 0.0;

    /// The share of the file received, 0..1; zero when the length is unknown.
    [[nodiscard]] double fraction() const noexcept;
    /// Seconds left, when both the length and the speed are known.
    [[nodiscard]] std::optional<double> remaining_seconds() const noexcept;
};

/// The file name of a model inside the models directory. These are the names the catalog and
/// the recognisers use, so the downloader cannot invent a file nothing would open.
[[nodiscard]] std::string_view whisper_model_file_name(domain::ModelSize size);
[[nodiscard]] std::string_view parakeet_model_file_name(domain::ParakeetModelSize size);
[[nodiscard]] std::string_view gigaam_model_file_name(domain::GigaamModelSize size);

/// The address a file is fetched from.
[[nodiscard]] std::string model_download_url(ModelEngine engine, std::string_view file_name);

class ModelDownloadService {
public:
    ModelDownloadService(platform::HttpClient& http, std::filesystem::path models_directory);

    /// Downloads into `<models>/<file_name>.part`, renames it on success and removes it on any
    /// failure, so a broken transfer never leaves something that looks like a model.
    [[nodiscard]] platform::Status download(ModelEngine engine, std::string_view file_name,
        const std::function<void(const ModelDownloadProgress&)>& progress,
        const domain::CancellationToken& cancellation);

    /// The path the file ends up at (used by the caller to open the model afterwards).
    [[nodiscard]] std::filesystem::path path_for(std::string_view file_name) const;

private:
    platform::HttpClient& http_;
    std::filesystem::path models_directory_;
};

} // namespace voicetyper::core::support
