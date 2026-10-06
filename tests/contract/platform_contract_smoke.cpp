// Contract smoke test: every portable contract header must be self-contained,
// standard-C++-only, and expose the frozen constants the migration depends on.
//
// This is compile evidence, not behavior evidence. It proves the headers form a
// consistent, self-contained API surface; it does NOT prove feature parity.
// Parity requires the contract/differential tests and the Windows scenarios
// tracked in docs/migration/cpp/parity-ledger.md.
//
// Deliberately built without Qt: the target links only the standard-library-only
// domain/platform targets, so a GUI-off build stays Qt-free and a header that
// accidentally pulled in Qt or an OS SDK would fail to compile here.

#include "domain/audio_format.hpp"
#include "domain/cancellation.hpp"
#include "domain/error.hpp"
#include "domain/gamepad_binding.hpp"
#include "domain/hotkey_gesture.hpp"
#include "domain/settings.hpp"
#include "domain/version.hpp"

#include "platform/api/audio_capture.hpp"
#include "platform/api/audio_converter.hpp"
#include "platform/api/clipboard.hpp"
#include "platform/api/clock.hpp"
#include "platform/api/cpu_topology.hpp"
#include "platform/api/engine_registry.hpp"
#include "platform/api/file_system.hpp"
#include "platform/api/gamepad.hpp"
#include "platform/api/hotkeys.hpp"
#include "platform/api/http.hpp"
#include "platform/api/lifecycle.hpp"
#include "platform/api/logger.hpp"
#include "platform/api/microphone.hpp"
#include "platform/api/model_store.hpp"
#include "platform/api/paste.hpp"
#include "platform/api/paths.hpp"
#include "platform/api/status_overlay.hpp"
#include "platform/api/transcriber.hpp"
#include "platform/api/tray.hpp"
#include "platform/api/updater.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <type_traits>

namespace domain = voicetyper::domain;
namespace platform = voicetyper::platform;

namespace {

int g_failures = 0;

void check(bool condition, const char* what)
{
    if (!condition) {
        ++g_failures;
        std::printf("FAIL %s\n", what);
    }
}

// --- Frozen contract constants, asserted at compile time -------------------

// Settings: 21 persisted properties, exact defaults from
// docs/migration/cpp/compatibility-contracts.md §1.
static_assert(domain::kAppSettingsPropertyCount == 21);
static_assert(domain::kDefaultRecordHotkey == "Ctrl+Alt+Space");
static_assert(domain::kDefaultCancelHotkey == "Ctrl+Alt+Escape");
static_assert(domain::kDefaultTermsDictionary == "API,CPU,GPU,ASR,STT,TTS,LLM,JSON,IDE,SQL");
static_assert(domain::kDefaultSilenceThresholdMs == 1200);
static_assert(domain::kSilenceThresholdMsMin == 300);
static_assert(domain::kSilenceThresholdMsMax == 10000);
static_assert(domain::kSettingsAutosaveDebounceMs == 700);

// Enum ordinals must match the .NET enums byte for byte.
static_assert(static_cast<std::uint8_t>(domain::RecordingMode::push_to_talk) == 0);
static_assert(static_cast<std::uint8_t>(domain::RecordingMode::toggle) == 1);
static_assert(static_cast<std::uint8_t>(domain::RecordingMode::vad) == 2);
static_assert(static_cast<std::uint8_t>(domain::AppTheme::system) == 0);
static_assert(static_cast<std::uint8_t>(domain::ModelSize::small) == 2);
static_assert(static_cast<std::uint8_t>(domain::TranscriptionEngine::whisper) == 0);
static_assert(static_cast<std::uint8_t>(domain::TranscriptionEngine::parakeet) == 1);
static_assert(static_cast<std::uint8_t>(domain::ParakeetModelSize::q8_0) == 3);
static_assert(static_cast<std::uint8_t>(domain::RecognitionLanguage::automatic) == 0);
static_assert(static_cast<std::uint8_t>(domain::AppLanguage::ru) == 0);

// Enum wire spellings (camelCase, exactly as System.Text.Json writes them).
static_assert(domain::to_wire(domain::RecordingMode::push_to_talk) == "pushToTalk");
static_assert(domain::to_wire(domain::TranscriptionEngine::parakeet) == "parakeet");
static_assert(domain::to_wire(domain::ParakeetModelSize::q4k) == "q4K");
static_assert(domain::to_wire(domain::ParakeetModelSize::q8_0) == "q8_0");
static_assert(domain::to_wire(domain::RecognitionLanguage::automatic) == "auto");
static_assert(domain::to_wire(domain::AppTheme::system) == "system");

// Hotkey modifier bits equal the .NET [Flags] values.
static_assert(static_cast<std::uint8_t>(domain::HotkeyModifiers::none) == 0);
static_assert(static_cast<std::uint8_t>(domain::HotkeyModifiers::alt) == 1);
static_assert(static_cast<std::uint8_t>(domain::HotkeyModifiers::control) == 2);
static_assert(static_cast<std::uint8_t>(domain::HotkeyModifiers::shift) == 4);
static_assert(static_cast<std::uint8_t>(domain::HotkeyModifiers::win) == 8);

// Audio: PCM16 / mono / 16 kHz is the transcription format (contracts §5).
static_assert(domain::kWhisperSampleRate == 16000);
static_assert(domain::kWhisperChannelCount == 1);
static_assert(domain::kWhisperBitsPerSample == 16);
static_assert(platform::target_transcription_format().is_whisper_input());
static_assert(domain::kWavHeaderBytes == 44);

// Gamepad grammar tokens (contracts §6). The grammar itself is a domain
// concern; src/platform/api/gamepad.hpp is the platform half.
static_assert(domain::kGamepadXInputToken == "XInput");
static_assert(domain::kGamepadDirectInputToken == "DInput");
static_assert(domain::kGamepadDirectInputPartCount == 3);

// Clipboard/paste, model download and logging constants (contracts §3, §4, §7).
static_assert(platform::kPasteDelay.count() == 80);
static_assert(platform::kClipboardMaxAttempts == 5);
static_assert(platform::kClipboardRetryDelayMs == 120);
static_assert(platform::kModelDownloadTimeoutMinutes == 30);
static_assert(platform::kModelProgressIntervalMs == 120);
static_assert(platform::kModelDownloadBufferBytes == 128 * 1024);
static_assert(platform::kLogRotateThresholdBytes == 1000000);
static_assert(platform::kLogArchiveCount == 5);
static_assert(platform::kHttpDefaultTimeout.count() == 1800);
static_assert(platform::kHttpUserAgent == "VoiceTyper/1.0");
static_assert(platform::kWhisperModelBaseUrl == "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/");
static_assert(platform::kVadModelBaseUrl == "https://huggingface.co/ggml-org/whisper-vad/resolve/main/");
static_assert(platform::kParakeetModelBaseUrl == "https://huggingface.co/mudler/parakeet-cpp-gguf/resolve/main/");
static_assert(platform::kParakeetAbiVersion == 6);
static_assert(platform::kParakeetPinnedCommit == "e75de9b6b9b688fd293aa22f7e27aa724ea286f8");
static_assert(platform::kSingleInstanceMutexName == "Global\\VoiceTyper_SingleInstance");
static_assert(platform::kAutostartRegistryValueName == "VoiceTyper");
static_assert(platform::kInstallerFileNameFormat == "VoiceTyper-{}-Setup.exe");
static_assert(platform::kOverlayPulsePeriod.count() == 350);
static_assert(platform::kGamepadPollIntervalMs == 33);
static_assert(platform::kVadPollIntervalMs == 250);
static_assert(platform::kVadMinSpeechMs == 250);
static_assert(platform::kVadNoSpeechStopMs == 5000);
static_assert(platform::kInferenceThreadsMin == 1);
static_assert(platform::kInferenceThreadsMax == 16);
static_assert(platform::kLogViewTailLines == 200);

// Result/Status are value types, not exception carriers.
static_assert(std::is_default_constructible_v<domain::Result<void>>);
static_assert(std::is_copy_constructible_v<domain::Status>);
static_assert(!std::is_convertible_v<domain::Status, bool>, "Status is not silently usable as a bool");

void check_error_contract()
{
    const auto failed = domain::Result<int>::failure(domain::ErrorCode::not_found, "missing");
    check(failed.is_error(), "Result<T>::failure is an error");
    check(failed.code() == domain::ErrorCode::not_found, "Result<T> keeps the code");
    check(domain::Result<void>::success().is_ok(), "Result<void>::success is ok");
    check(domain::check_cancelled(domain::CancellationToken{}).is_ok(), "default token is not cancelled");

    domain::CancellationSource source;
    check(!source.token().is_cancellation_requested(), "fresh source is not cancelled");
    source.request_cancellation();
    check(domain::check_cancelled(source.token()).code() == domain::ErrorCode::cancelled, "cancel is observed");
}

void check_settings_contract()
{
    const auto defaults = domain::AppSettings::defaults();
    check(defaults.recording_mode == domain::RecordingMode::push_to_talk, "default recording mode");
    check(defaults.language == domain::RecognitionLanguage::ru, "default language");
    check(defaults.model_size == domain::ModelSize::small, "default model size");
    check(defaults.transcription_engine == domain::TranscriptionEngine::whisper, "default engine");
    check(defaults.parakeet_model_size == domain::ParakeetModelSize::q8_0, "default parakeet size");
    check(defaults.auto_paste_enabled, "auto paste defaults to on");
    check(defaults.silence_threshold_ms == 1200, "default silence threshold");
    check(!defaults.record_gamepad_button.has_value(), "record gamepad defaults to null");
    check(!defaults.microphone_device_id.has_value(), "microphone id defaults to null");
    check(defaults.validate().is_ok(), "defaults validate");

    check(!defaults.record_hotkey_gesture().is_error(), "default record hotkey parses");
    check(defaults.record_gamepad_binding().is_ok(), "absent gamepad binding is not an error");
    check(domain::recognition_language_from_wire("AUTO").has_value(), "enum read is case-insensitive");
    check(!domain::recognition_language_from_wire("de").has_value(), "unknown enum read fails");
}

void check_hotkey_contract()
{
    const auto gesture = domain::parse_hotkey("ctrl+alt+space");
    check(gesture.is_ok(), "hotkey parses");
    check(gesture.value().to_string() == "Ctrl+Alt+Space", "hotkey canonicalizes");
    check(domain::parse_hotkey("f12").is_ok(), "bare function key parses");
    check(domain::parse_hotkey("").is_error(), "empty hotkey is rejected");
    check(domain::parse_hotkey("Ctrl+Alt").is_error(), "modifier-only hotkey is rejected");

    const domain::HotkeyGesture function_key{domain::HotkeyModifiers::none, "F12"};
    check(function_key.is_capturable(), "F12 needs no modifier");
    const domain::HotkeyGesture bare_key{domain::HotkeyModifiers::none, "Space"};
    check(!bare_key.is_capturable(), "Space requires a modifier to be capturable");
}

void check_gamepad_contract()
{
    check(domain::kGamepadXInputToken == "XInput", "xinput token");
    const domain::GamepadBinding xinput{domain::GamepadSource::xinput, "A"};
    check(xinput.to_string() == "XInput|A", "xinput binding renders");
    const domain::GamepadBinding dinput{domain::GamepadSource::directinput, "Logitech|3"};
    check(dinput.to_string() == "DInput|Logitech|3", "directinput binding renders");
    check(domain::x_input_button_from_name("dpadup").has_value(), "button lookup is case-insensitive");
    check(!domain::x_input_button_from_name("Nonsense").has_value(), "unknown button lookup fails");
}

void check_audio_contract()
{
    const auto device_format = domain::AudioFormat(48000, 2, domain::SampleFormat::pcm_s16);
    check(device_format.validate().is_ok(), "48 kHz stereo PCM16 is valid");
    check(device_format.bytes_per_frame() == 4, "stereo PCM16 frame is 4 bytes");
    check(device_format.frame_count(1024) == 256, "frame count divides");
    check(device_format.validate_buffer(1023).code() == domain::ErrorCode::corrupt_data, "partial frame rejected");
    check(domain::AudioFormat{}.validate().is_error(), "default format is invalid");
}

void check_updater_contract()
{
    check(platform::compare_update_versions("1.2.0", "1.1.9") > 0, "newer numeric core wins");
    check(platform::compare_update_versions("1.0.0", "1.0.0-rc1") > 0, "stable beats prerelease");
    check(platform::compare_update_versions("1.0.0-rc1", "1.0.0-rc2") == 0, "prerelease identifiers ignored");
    check(platform::compare_update_versions("1.0.0+build", "1.0.0") == 0, "build metadata ignored");
    check(platform::strip_version_tag_prefix("v1.1.3") == "1.1.3", "tag prefix stripped");
    check(!platform::extract_release_sha256("no marker here").has_value(), "absent sha marker");
    check(platform::extract_release_sha256("SHA-256: " + std::string(64, 'a')).has_value(), "sha marker found");
}

void check_cpu_topology_contract()
{
    platform::CpuTopology known;
    known.logical_processors = 8;
    known.physical_cores = 4;
    known.physical_cores_known = true;
    check(platform::inference_thread_count(known) == 4, "inference threads follow physical cores");

    platform::CpuTopology many;
    many.logical_processors = 64;
    many.physical_cores = 32;
    many.physical_cores_known = true;
    check(platform::inference_thread_count(many) == 16, "inference threads clamp to 16");

    platform::CpuTopology unknown;
    unknown.logical_processors = 8;
    check(platform::physical_cores_fallback(8) == 4, "fallback is half the logical count");
    check(platform::inference_thread_count(unknown) == 4, "unknown topology uses the fallback");
    check(platform::vad_thread_count(unknown) == 4, "vad threads are half the logical count");
}

} // namespace

int main()
{
    check(!domain::version().empty(), "domain version is available");

    check_error_contract();
    check_settings_contract();
    check_hotkey_contract();
    check_gamepad_contract();
    check_audio_contract();
    check_updater_contract();
    check_cpu_topology_contract();

    if (g_failures != 0) {
        std::printf("platform-contract-smoke: %d check(s) failed\n", g_failures);
        return 1;
    }

    std::printf("platform-contract-smoke: OK (portable contract headers, Qt-free)\n");
    return 0;
}
