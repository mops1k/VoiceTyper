#include "platform/api/model_store.hpp"

#include <array>
#include <string>

namespace voicetyper::platform {
namespace {

ModelDescriptor make_descriptor(
    ModelKind kind,
    ModelSize size,
    ParakeetModelSize parakeet_size,
    std::string_view base_url,
    std::string_view file_name,
    std::uint64_t expected_bytes)
{
    ModelDescriptor descriptor;
    descriptor.kind = kind;
    descriptor.size = size;
    descriptor.parakeet_size = parakeet_size;
    descriptor.file_name = std::string(file_name);
    descriptor.download_url = std::string(base_url) + descriptor.file_name;
    descriptor.expected_bytes = expected_bytes;
    return descriptor;
}

const std::array<ModelDescriptor, 5> kWhisperCatalog{
    make_descriptor(ModelKind::whisper, ModelSize::tiny, ParakeetModelSize::q8_0, kWhisperModelBaseUrl, "ggml-tiny-q8_0.bin", 43'537'433),
    make_descriptor(ModelKind::whisper, ModelSize::base, ParakeetModelSize::q8_0, kWhisperModelBaseUrl, "ggml-base-q8_0.bin", 81'768'585),
    make_descriptor(ModelKind::whisper, ModelSize::small, ParakeetModelSize::q8_0, kWhisperModelBaseUrl, "ggml-small-q8_0.bin", 264'464'607),
    make_descriptor(ModelKind::whisper, ModelSize::medium, ParakeetModelSize::q8_0, kWhisperModelBaseUrl, "ggml-medium-q8_0.bin", 823'369'779),
    make_descriptor(ModelKind::whisper, ModelSize::large, ParakeetModelSize::q8_0, kWhisperModelBaseUrl, "ggml-large-v3-turbo-q8_0.bin", 874'188'075),
};

const std::array<ModelDescriptor, 1> kVadCatalog{
    make_descriptor(ModelKind::vad, ModelSize::small, ParakeetModelSize::q8_0, kVadModelBaseUrl, "ggml-silero-v6.2.0.bin", 885'098),
};

const std::array<ModelDescriptor, 4> kParakeetCatalog{
    make_descriptor(ModelKind::parakeet, ModelSize::small, ParakeetModelSize::q4k, kParakeetModelBaseUrl, "tdt-0.6b-v3-q4_k.gguf", 675'200'864),
    make_descriptor(ModelKind::parakeet, ModelSize::small, ParakeetModelSize::q5k, kParakeetModelBaseUrl, "tdt-0.6b-v3-q5_k.gguf", 741'867'360),
    make_descriptor(ModelKind::parakeet, ModelSize::small, ParakeetModelSize::q6k, kParakeetModelBaseUrl, "tdt-0.6b-v3-q6_k.gguf", 812'700'512),
    make_descriptor(ModelKind::parakeet, ModelSize::small, ParakeetModelSize::q8_0, kParakeetModelBaseUrl, "tdt-0.6b-v3-q8_0.gguf", 940'663'680),
};

/// GigaAM rows carry the quant instead of a Parakeet size. File names and byte
/// counts are exact and were verified against the HuggingFace API on
/// 2026-10-06 (HF LFS metadata), not estimated from the published MiB figure.
ModelDescriptor make_gigaam_descriptor(
    domain::GigaamModelSize size,
    std::string_view file_name,
    std::uint64_t expected_bytes)
{
    ModelDescriptor descriptor;
    descriptor.kind = ModelKind::gigaam;
    descriptor.size = ModelSize::small;
    descriptor.parakeet_size = ParakeetModelSize::q8_0;
    descriptor.gigaam_size = size;
    descriptor.file_name = std::string(file_name);
    descriptor.download_url = std::string(kGigaamModelBaseUrl) + descriptor.file_name;
    descriptor.expected_bytes = expected_bytes;
    return descriptor;
}

const std::array<ModelDescriptor, 4> kGigaamCatalog{
    make_gigaam_descriptor(domain::GigaamModelSize::q4_k_m, "gigaam-v3-e2e-rnnt-Q4_K_M.gguf", 183'948'704),
    make_gigaam_descriptor(domain::GigaamModelSize::q5_k_m, "gigaam-v3-e2e-rnnt-Q5_K_M.gguf", 206'392'736),
    make_gigaam_descriptor(domain::GigaamModelSize::q6_k, "gigaam-v3-e2e-rnnt-Q6_K.gguf", 227'953'952),
    make_gigaam_descriptor(domain::GigaamModelSize::q8_0, "gigaam-v3-e2e-rnnt-Q8_0.gguf", 273'724'832),
};

const std::vector<std::string> kLegacyModelFileNames{
    "ggml-tiny.bin",
    "ggml-base.bin",
    "ggml-small.bin",
    "ggml-medium.bin",
    "ggml-large-v3-turbo.bin",
    "ggml-tiny-q5_1.bin",
    "ggml-base-q5_1.bin",
    "ggml-small-q5_1.bin",
    "ggml-medium-q5_0.bin",
    "ggml-large-v3-turbo-q5_0.bin",
};

} // namespace

bool ModelDescriptor::matches_path(const std::filesystem::path& path) const
{
    return path.filename() == std::filesystem::path(file_name);
}

const std::array<ModelDescriptor, 5>& whisper_catalog()
{
    return kWhisperCatalog;
}

const std::array<ModelDescriptor, 1>& vad_catalog()
{
    return kVadCatalog;
}

const std::array<ModelDescriptor, 4>& parakeet_catalog()
{
    return kParakeetCatalog;
}

const std::array<ModelDescriptor, 4>& gigaam_catalog()
{
    return kGigaamCatalog;
}

Result<ModelDescriptor> find_whisper_model(ModelSize size)
{
    const auto index = static_cast<std::size_t>(size);
    if (index >= kWhisperCatalog.size()) {
        return Result<ModelDescriptor>::failure(
            ErrorCode::invalid_argument, "unknown whisper model size");
    }
    return kWhisperCatalog[index];
}

Result<ModelDescriptor> find_vad_model()
{
    return kVadCatalog[0];
}

Result<ModelDescriptor> find_parakeet_model(ParakeetModelSize size)
{
    const auto index = static_cast<std::size_t>(size);
    if (index >= kParakeetCatalog.size()) {
        return Result<ModelDescriptor>::failure(
            ErrorCode::invalid_argument, "unknown parakeet model size");
    }
    return kParakeetCatalog[index];
}

Result<ModelDescriptor> find_gigaam_model(domain::GigaamModelSize size)
{
    const auto index = static_cast<std::size_t>(size);
    if (index >= kGigaamCatalog.size()) {
        return Result<ModelDescriptor>::failure(
            ErrorCode::invalid_argument, "unknown gigaam model size");
    }
    return kGigaamCatalog[index];
}

const std::vector<std::string>& legacy_model_file_names()
{
    return kLegacyModelFileNames;
}

} // namespace voicetyper::platform
