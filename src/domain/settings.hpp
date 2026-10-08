#pragma once

// Settings contract: the exact persisted shape of settings.json.
//
// Evidence: docs/migration/cpp/compatibility-contracts.md §1 "Settings JSON" and
// VoiceTyper.Core/Models/AppSettings.cs + VoiceTyper.Core/Services/SettingsService.cs.
//
// What is frozen here:
//   * the 23 property names, in AppSettings *declaration order*, which is also
//     the current serializer output order (byte fixtures depend on it). 21 of them
//     are the .NET properties; gigaamModelSize and bestOf are C++-only extensions
//     the .NET serializer ignores on read;
//   * the camelCase wire spelling of every enum value;
//   * every default value;
//   * the nullability of recordGamepadButton / cancelGamepadButton /
//     microphoneDeviceId (emitted as JSON null, never omitted).
//
// What is deliberately NOT decided here: parsing/serialization, case-insensitive
// read policy, unknown-field handling, integer-enum acceptance, atomic tmp+replace
// and the 700 ms autosave debounce are Phase B (settings/storage) work with its
// own golden fixtures. This header is the data contract only.

#include "domain/error.hpp"
#include "domain/gamepad_binding.hpp"
#include "domain/hotkey_gesture.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace voicetyper::domain {

/// How a recording session is started and stopped.
enum class RecordingMode : std::uint8_t {
    /// Hold the hotkey; recording runs while it is held.
    push_to_talk = 0,
    /// Press to start, press again to stop.
    toggle = 1,
    /// Automatic stop driven by VAD silence detection.
    vad = 2,
};

/// Application appearance theme.
enum class AppTheme : std::uint8_t {
    /// Follow the OS theme.
    system = 0,
    light = 1,
    dark = 2,
};

/// Whisper ggml model size.
enum class ModelSize : std::uint8_t {
    tiny = 0,
    base = 1,
    small = 2,
    medium = 3,
    large = 4,
};

/// Selected speech-to-text engine. The default is whisper (ordinal 0); C++
/// must never silently substitute one engine when another is unavailable.
///
/// `gigaam` is a C++-only extension beyond the .NET reference (decided by
/// Alexander 2026-10-06): the legacy .NET serializer cannot resolve this wire
/// value and falls back to a whole-document default, which is why a settings
/// backup is taken before a user switches engines. See
/// docs/migration/cpp/parity-ledger.md.
enum class TranscriptionEngine : std::uint8_t {
    whisper = 0,
    parakeet = 1,
    gigaam = 2,
};

/// Parakeet GGUF quantization. Ordinal 3 (q8_0) is the default.
enum class ParakeetModelSize : std::uint8_t {
    q4k = 0,
    q5k = 1,
    q6k = 2,
    q8_0 = 3,
};

/// GigaAM-v3 e2e-rnnt GGUF quantization
/// (handy-computer/gigaam-v3-e2e-rnnt-gguf). Ordinal 3 (q8_0) is the default:
/// the published FLEURS-ru WER of every quant is within 0.07 pp of the others
/// (5.35..5.42), so the reference quant is the honest default.
enum class GigaamModelSize : std::uint8_t {
    q4_k_m = 0,
    q5_k_m = 1,
    q6_k = 2,
    q8_0 = 3,
};

/// Recognition language. Ordinal 0 is the wire value "auto"; the enumerator is
/// named `automatic` because `auto` is a C++ keyword.
enum class RecognitionLanguage : std::uint8_t {
    automatic = 0,
    ru = 1,
    en = 2,
};

/// User interface language.
enum class AppLanguage : std::uint8_t {
    ru = 0,
    en = 1,
};

/// Exact camelCase wire spellings, as produced by System.Text.Json with a
/// camelCase naming policy. Reading is case-insensitive, so parsing lowercases
/// the input first.
inline constexpr std::string_view to_wire(RecordingMode value) noexcept
{
    switch (value) {
    case RecordingMode::push_to_talk: return "pushToTalk";
    case RecordingMode::toggle: return "toggle";
    case RecordingMode::vad: return "vad";
    }
    return "pushToTalk";
}

inline constexpr std::string_view to_wire(AppTheme value) noexcept
{
    switch (value) {
    case AppTheme::system: return "system";
    case AppTheme::light: return "light";
    case AppTheme::dark: return "dark";
    }
    return "system";
}

inline constexpr std::string_view to_wire(ModelSize value) noexcept
{
    switch (value) {
    case ModelSize::tiny: return "tiny";
    case ModelSize::base: return "base";
    case ModelSize::small: return "small";
    case ModelSize::medium: return "medium";
    case ModelSize::large: return "large";
    }
    return "small";
}

inline constexpr std::string_view to_wire(TranscriptionEngine value) noexcept
{
    switch (value) {
    case TranscriptionEngine::whisper: return "whisper";
    case TranscriptionEngine::parakeet: return "parakeet";
    case TranscriptionEngine::gigaam: return "gigaam";
    }
    return "whisper";
}

inline constexpr std::string_view to_wire(GigaamModelSize value) noexcept
{
    switch (value) {
    case GigaamModelSize::q4_k_m: return "q4_k_m";
    case GigaamModelSize::q5_k_m: return "q5_k_m";
    case GigaamModelSize::q6_k: return "q6_k";
    case GigaamModelSize::q8_0: return "q8_0";
    }
    return "q8_0";
}

inline constexpr std::string_view to_wire(ParakeetModelSize value) noexcept
{
    switch (value) {
    case ParakeetModelSize::q4k: return "q4K";
    case ParakeetModelSize::q5k: return "q5K";
    case ParakeetModelSize::q6k: return "q6K";
    case ParakeetModelSize::q8_0: return "q8_0";
    }
    return "q8_0";
}

inline constexpr std::string_view to_wire(RecognitionLanguage value) noexcept
{
    switch (value) {
    case RecognitionLanguage::automatic: return "auto";
    case RecognitionLanguage::ru: return "ru";
    case RecognitionLanguage::en: return "en";
    }
    return "ru";
}

inline constexpr std::string_view to_wire(AppLanguage value) noexcept
{
    switch (value) {
    case AppLanguage::ru: return "ru";
    case AppLanguage::en: return "en";
    }
    return "ru";
}

/// Case-insensitive wire-name lookup for every settings enum.
/// An unknown name yields nullopt; the settings layer then falls back to the
/// property default, exactly like an invalid enum string in System.Text.Json.
[[nodiscard]] std::optional<RecordingMode> recording_mode_from_wire(std::string_view name);
[[nodiscard]] std::optional<AppTheme> app_theme_from_wire(std::string_view name);
[[nodiscard]] std::optional<ModelSize> model_size_from_wire(std::string_view name);
[[nodiscard]] std::optional<TranscriptionEngine> transcription_engine_from_wire(std::string_view name);
[[nodiscard]] std::optional<ParakeetModelSize> parakeet_model_size_from_wire(std::string_view name);
[[nodiscard]] std::optional<GigaamModelSize> gigaam_model_size_from_wire(std::string_view name);
[[nodiscard]] std::optional<RecognitionLanguage> recognition_language_from_wire(std::string_view name);
[[nodiscard]] std::optional<AppLanguage> app_language_from_wire(std::string_view name);

/// Number of persisted properties, in declaration order. Golden fixtures assert
/// this value; changing it is a schema change.
inline constexpr std::size_t kAppSettingsPropertyCount = 23;

/// Default hotkey that starts/stops recording.
inline constexpr std::string_view kDefaultRecordHotkey = "Ctrl+Alt+Space";
/// Default hotkey that cancels recording/processing.
inline constexpr std::string_view kDefaultCancelHotkey = "Ctrl+Alt+Escape";
/// Default technical-terms prompt list.
inline constexpr std::string_view kDefaultTermsDictionary = "API,CPU,GPU,ASR,STT,TTS,LLM,JSON,IDE,SQL";
/// Default VAD silence threshold, milliseconds.
inline constexpr int kDefaultSilenceThresholdMs = 1200;
/// Inclusive bounds the UI clamps silenceThresholdMs to before saving. A direct
/// service-level save does not clamp in the current .NET build; the C++ settings
/// layer keeps those two paths distinguishable.
inline constexpr int kSilenceThresholdMsMin = 300;
inline constexpr int kSilenceThresholdMsMax = 10000;
/// Inclusive bounds for `bestOf`, the number of recognition candidates the engine
/// compares for the final result. They repeat the engine's own limits on purpose
/// (asr/engine_parameters.hpp kMinBestOf = 1, kMaxBestOf = 8): the domain layer
/// must not include asr/, and the engine has to have the last word anyway, so the
/// app clamps with its own copy on the way in.
inline constexpr int kBestOfMin = 1;
inline constexpr int kBestOfMax = 8;
/// Default `bestOf`: the same 3 greedy candidates the .NET state machine always
/// asked for (domain::kFinalBestOf in recording_state_machine.hpp, which is where
/// a static_assert keeps the two in step). The number is repeated here because
/// that header includes this one and cannot be included back.
inline constexpr int kDefaultBestOf = 3;
/// UI autosave debounce, milliseconds.
inline constexpr int kSettingsAutosaveDebounceMs = 700;

/// The persisted application settings.
///
/// Ownership/threading: plain value type. The UI layer owns the authoritative
/// instance; worker threads receive copies or immutable snapshots.
struct AppSettings {
    // --- Declaration order below IS the settings.json output order. Do not
    // --- reorder: the golden fixture "settings-defaults" is byte-compared.

    /// JSON: recordingMode. Default "pushToTalk".
    RecordingMode recording_mode = RecordingMode::push_to_talk;
    /// JSON: recordHotkey. Default "Ctrl+Alt+Space".
    std::string record_hotkey{kDefaultRecordHotkey};
    /// JSON: cancelHotkey. Default "Ctrl+Alt+Escape".
    std::string cancel_hotkey{kDefaultCancelHotkey};
    /// JSON: recordGamepadButton. Default null (unassigned).
    std::optional<std::string> record_gamepad_button;
    /// JSON: cancelGamepadButton. Default null (unassigned).
    std::optional<std::string> cancel_gamepad_button;
    /// JSON: language. Default "ru".
    RecognitionLanguage language = RecognitionLanguage::ru;
    /// JSON: modelSize. Default "small".
    ModelSize model_size = ModelSize::small;
    /// JSON: transcriptionEngine. Default "whisper".
    TranscriptionEngine transcription_engine = TranscriptionEngine::whisper;
    /// JSON: parakeetModelSize. Default "q8_0".
    ParakeetModelSize parakeet_model_size = ParakeetModelSize::q8_0;
    /// JSON: gigaamModelSize. Default "q8_0".
    ///
    /// C++-only extension: the legacy .NET serializer has no such property and
    /// ignores it on read, so a .NET rollback keeps working as long as
    /// transcriptionEngine is not "gigaam" (see the enum above).
    GigaamModelSize gigaam_model_size = GigaamModelSize::q8_0;
    /// JSON: autoPasteEnabled. Default true.
    bool auto_paste_enabled = true;
    /// JSON: termsDictionary. Default "API,CPU,GPU,ASR,STT,TTS,LLM,JSON,IDE,SQL".
    std::string terms_dictionary{kDefaultTermsDictionary};
    /// JSON: silenceThresholdMs. Default 1200.
    int silence_threshold_ms = kDefaultSilenceThresholdMs;
    /// JSON: startWithWindows. Default false.
    bool start_with_windows = false;
    /// JSON: startMinimized. Default false.
    bool start_minimized = false;
    /// JSON: theme. Default "system".
    AppTheme theme = AppTheme::system;
    /// JSON: hideOnFocusLoss. Default false.
    ///
    /// Intent-parity note: in the current .NET build the setting is bound and
    /// persisted but has no consumer, so hiding does not happen. The C++ plan
    /// requires an explicit product decision here; this contract only records
    /// that the value must round-trip through save/load unchanged.
    bool hide_on_focus_loss = false;
    /// JSON: appLanguage. Default "ru".
    AppLanguage app_language = AppLanguage::ru;
    /// JSON: noiseReductionEnabled. Default false.
    bool noise_reduction_enabled = false;
    /// JSON: temperature. Default 0.0.
    double temperature = 0.0;
    /// JSON: bestOf. Default 3 (kDefaultBestOf), the value the .NET machine
    /// always asked for.
    ///
    /// C++-only extension: the .NET serializer has no such property, so a rollback
    /// ignores it on read and falls back to the machine's own 3 candidates instead
    /// of breaking dictation. Written next to temperature because both are the
    /// decoder's generation parameters; the engine accepts 1..8 (asr/engine_parameters.hpp
    /// kMinBestOf/kMaxBestOf) and out-of-range values fail validate() and are
    /// clamped before they reach the engine.
    int best_of = kDefaultBestOf;
    /// JSON: conditionOnPreviousText. Default false.
    ///
    /// Intent-parity note: in the current .NET build this only selects
    /// `WithNoContext()` when false; no previous transcript is stored or passed.
    /// The C++ plan must not claim implemented context without a new
    /// contract/test, so the value is stored but not yet acted upon.
    bool condition_on_previous_text = false;
    /// JSON: microphoneDeviceId. Default null (use the default device).
    ///
    /// Intent-parity note: the current SettingsViewModel does not load this
    /// value into the UI. The C++ plan requires the user's selection to be
    /// preserved; that is a Phase B/D behavior change with a regression test,
    /// not a contract-header decision.
    std::optional<std::string> microphone_device_id;

    /// The value every property takes when settings.json is missing, corrupt,
    /// unreadable or rejected by the serializer. Exactly the "settings-defaults"
    /// golden fixture plus the C++-only extensions (gigaamModelSize, bestOf).
    [[nodiscard]] static AppSettings defaults() { return AppSettings{}; }

    /// Deep copy. Mirrors the .NET `AppSettings.Clone()` contract: independent
    /// optional/string storage, no shared state.
    [[nodiscard]] AppSettings clone() const { return *this; }

    /// Checks the documented bounds without modifying anything.
    /// The UI-save clamp (300..10000) is applied by the settings layer, not here,
    /// so that "validate" and "canonicalize" stay distinguishable.
    [[nodiscard]] Status validate() const;

    /// Parses the two hotkey strings with the documented grammar.
    /// Invalid strings produce ErrorCode::invalid_argument and are *not* silently
    /// replaced, so the settings layer can decide the recovery policy.
    [[nodiscard]] Result<HotkeyGesture> record_hotkey_gesture() const
    {
        return parse_hotkey(record_hotkey);
    }

    /// Parses the cancel hotkey string; see record_hotkey_gesture().
    [[nodiscard]] Result<HotkeyGesture> cancel_hotkey_gesture() const
    {
        return parse_hotkey(cancel_hotkey);
    }

    /// Parses recordGamepadButton. A nullopt binding is an unassigned action and
    /// is a success, not an error.
    [[nodiscard]] Result<GamepadBinding> record_gamepad_binding() const;

    /// Parses cancelGamepadButton; see record_gamepad_binding().
    [[nodiscard]] Result<GamepadBinding> cancel_gamepad_binding() const;
};

// ---------------------------------------------------------------------------
// Inline implementations.
//
// The wire-name lookups and the bounds check are part of the data contract and
// are pure, so they are defined here rather than deferred. Parsing/serializing
// settings.json itself is Phase B work.
// ---------------------------------------------------------------------------

namespace detail {

/// Case-insensitive lookup in a table of (wire name, value) pairs.
template <typename T, std::size_t N>
[[nodiscard]] std::optional<T> find_wire(std::string_view name, const std::pair<std::string_view, T> (&table)[N])
{
    for (const auto& [wire, value] : table) {
        if (equals_ignore_ascii_case(name, wire)) {
            return value;
        }
    }
    return std::nullopt;
}

} // namespace detail

inline std::optional<RecordingMode> recording_mode_from_wire(std::string_view name)
{
    static constexpr std::pair<std::string_view, RecordingMode> table[]{
        {"pushToTalk", RecordingMode::push_to_talk},
        {"toggle", RecordingMode::toggle},
        {"vad", RecordingMode::vad},
    };
    return detail::find_wire(name, table);
}

inline std::optional<AppTheme> app_theme_from_wire(std::string_view name)
{
    static constexpr std::pair<std::string_view, AppTheme> table[]{
        {"system", AppTheme::system},
        {"light", AppTheme::light},
        {"dark", AppTheme::dark},
    };
    return detail::find_wire(name, table);
}

inline std::optional<ModelSize> model_size_from_wire(std::string_view name)
{
    static constexpr std::pair<std::string_view, ModelSize> table[]{
        {"tiny", ModelSize::tiny},
        {"base", ModelSize::base},
        {"small", ModelSize::small},
        {"medium", ModelSize::medium},
        {"large", ModelSize::large},
    };
    return detail::find_wire(name, table);
}

inline std::optional<TranscriptionEngine> transcription_engine_from_wire(std::string_view name)
{
    static constexpr std::pair<std::string_view, TranscriptionEngine> table[]{
        {"whisper", TranscriptionEngine::whisper},
        {"parakeet", TranscriptionEngine::parakeet},
        {"gigaam", TranscriptionEngine::gigaam},
    };
    return detail::find_wire(name, table);
}

inline std::optional<GigaamModelSize> gigaam_model_size_from_wire(std::string_view name)
{
    static constexpr std::pair<std::string_view, GigaamModelSize> table[]{
        {"q4_k_m", GigaamModelSize::q4_k_m},
        {"q5_k_m", GigaamModelSize::q5_k_m},
        {"q6_k", GigaamModelSize::q6_k},
        {"q8_0", GigaamModelSize::q8_0},
    };
    return detail::find_wire(name, table);
}

inline std::optional<ParakeetModelSize> parakeet_model_size_from_wire(std::string_view name)
{
    static constexpr std::pair<std::string_view, ParakeetModelSize> table[]{
        {"q4K", ParakeetModelSize::q4k},
        {"q5K", ParakeetModelSize::q5k},
        {"q6K", ParakeetModelSize::q6k},
        {"q8_0", ParakeetModelSize::q8_0},
    };
    return detail::find_wire(name, table);
}

inline std::optional<RecognitionLanguage> recognition_language_from_wire(std::string_view name)
{
    static constexpr std::pair<std::string_view, RecognitionLanguage> table[]{
        {"auto", RecognitionLanguage::automatic},
        {"ru", RecognitionLanguage::ru},
        {"en", RecognitionLanguage::en},
    };
    return detail::find_wire(name, table);
}

inline std::optional<AppLanguage> app_language_from_wire(std::string_view name)
{
    static constexpr std::pair<std::string_view, AppLanguage> table[]{
        {"ru", AppLanguage::ru},
        {"en", AppLanguage::en},
    };
    return detail::find_wire(name, table);
}

inline Status AppSettings::validate() const
{
    if (silence_threshold_ms < kSilenceThresholdMsMin || silence_threshold_ms > kSilenceThresholdMsMax) {
        return Status::failure(
            ErrorCode::out_of_range,
            "silenceThresholdMs must be within the UI clamp range");
    }
    if (temperature < 0.0) {
        return Status::failure(ErrorCode::out_of_range, "temperature must not be negative");
    }
    if (best_of < kBestOfMin || best_of > kBestOfMax) {
        return Status::failure(
            ErrorCode::out_of_range,
            "bestOf must be within the engine range");
    }
    return Status::success();
}

namespace detail {

/// Parses one gamepad binding string under the frozen 3-part grammar:
/// "XInput|<Button>" or "DInput|<ProductName>|<zeroBasedIndex>".
[[nodiscard]] inline Result<GamepadBinding> parse_gamepad_binding(std::string_view text)
{
    if (text.empty()) {
        return Result<GamepadBinding>::failure(ErrorCode::invalid_argument, "gamepad binding is empty");
    }

    const std::size_t first_separator = text.find('|');
    if (first_separator == std::string_view::npos) {
        return Result<GamepadBinding>::failure(
            ErrorCode::invalid_argument, "gamepad binding has no source part");
    }

    const std::string_view source_token = text.substr(0, first_separator);
    const std::string_view rest = text.substr(first_separator + 1);

    if (equals_ignore_ascii_case(source_token, kGamepadXInputToken)) {
        if (rest.empty()) {
            return Result<GamepadBinding>::failure(
                ErrorCode::invalid_argument, "XInput binding has no button name");
        }
        if (rest.find('|') != std::string_view::npos) {
            return Result<GamepadBinding>::failure(
                ErrorCode::invalid_argument, "XInput binding has too many parts");
        }
        if (!x_input_button_from_name(rest).has_value()) {
            return Result<GamepadBinding>::failure(
                ErrorCode::invalid_argument, "XInput button name is not a known button");
        }
        return GamepadBinding{GamepadSource::xinput, std::string(rest)};
    }

    if (equals_ignore_ascii_case(source_token, kGamepadDirectInputToken)) {
        // Exactly kGamepadDirectInputPartCount '|'-separated parts in total.
        std::size_t parts = 1;
        for (const char ch : rest) {
            if (ch == '|') {
                ++parts;
            }
        }
        if (parts != kGamepadDirectInputPartCount - 1) {
            return Result<GamepadBinding>::failure(
                ErrorCode::invalid_argument, "DirectInput binding must be DInput|Product|Index");
        }
        const std::size_t second_separator = rest.find('|');
        const std::string_view product = rest.substr(0, second_separator);
        const std::string_view index = rest.substr(second_separator + 1);
        if (product.empty() || index.empty()) {
            return Result<GamepadBinding>::failure(
                ErrorCode::invalid_argument, "DirectInput binding has an empty part");
        }
        return GamepadBinding{GamepadSource::directinput, std::string(rest)};
    }

    return Result<GamepadBinding>::failure(
        ErrorCode::invalid_argument, "gamepad binding source must be XInput or DInput");
}

} // namespace detail

inline Result<GamepadBinding> AppSettings::record_gamepad_binding() const
{
    if (!record_gamepad_button.has_value() || record_gamepad_button->empty()) {
        // Unassigned is a valid state, not a failure.
        return GamepadBinding{};
    }
    return detail::parse_gamepad_binding(*record_gamepad_button);
}

inline Result<GamepadBinding> AppSettings::cancel_gamepad_binding() const
{
    if (!cancel_gamepad_button.has_value() || cancel_gamepad_button->empty()) {
        return GamepadBinding{};
    }
    return detail::parse_gamepad_binding(*cancel_gamepad_button);
}

} // namespace voicetyper::domain
