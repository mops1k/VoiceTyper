// Fixture generator for the C++ settings contract (Phase A golden fixtures).
//
// It compiles the REAL product sources (VoiceTyper.Core/Models/AppSettings.cs and
// VoiceTyper.Core/Services/SettingsService.cs) into itself, so the documents below are
// produced by the real System.Text.Json with the real SettingsService.JsonOptions instance.
// Nothing here is hand-written JSON: every fixture is emitted by the serializer, and every
// expectation printed on stdout comes from running the real SettingsService.Load() over the
// generated file.
//
// Run via ../generate.sh (it wires the temporary project and the Windows run).

using System.Globalization;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.Json.Serialization;
using VoiceTyper.Core.Models;
using VoiceTyper.Core.Services;

// ---------------------------------------------------------------------------------------------
// Fixture 3 payload: pre-engine schema, property-for-property identical to
// git 1163045^:VoiceTyper.Core/Models/AppSettings.cs (commit "Add NVIDIA Parakeet ...
// transcription engine" is the commit that introduced TranscriptionEngine/ParakeetModelSize).
// Verified by comparing the reflected property list of this type against the property list
// extracted from that git blob; generate.sh re-runs that comparison.
// ---------------------------------------------------------------------------------------------
var legacySchema = typeof(LegacyAppSettings).GetProperties()
    .Select(p => p.Name)
    .ToArray();

var outputDirectory = args.Length > 0
    ? args[0]
    : throw new ArgumentException("usage: GenerateSettingsFixtures <output-directory>");

Directory.CreateDirectory(outputDirectory);

var options = SettingsService.JsonOptions;

// Pascal-case + unknown members: same document as settings-all-fields, written through an
// options clone with PropertyNamingPolicy = null (CLR names), then extended with members that
// do not exist in AppSettings. The values are byte-identical to settings-all-fields.json, so the
// C++ test can assert parsed(pascal-unknown) == parsed(all-fields) field by field.
var pascalOptions = new JsonSerializerOptions
{
    WriteIndented = options.WriteIndented,
    PropertyNamingPolicy = null,
    PropertyNameCaseInsensitive = options.PropertyNameCaseInsensitive,
    Converters = { new JsonStringEnumConverter(JsonNamingPolicy.CamelCase) },
};

var defaults = new AppSettings();

// Every field non-default; both nullable strings populated.
var allFields = new AppSettings
{
    RecordingMode = RecordingMode.Vad,
    RecordHotkey = "F12",
    CancelHotkey = "F11",
    RecordGamepadButton = "XInput|A",
    CancelGamepadButton = "DInput|Logitech|3",
    Language = RecognitionLanguage.En,
    ModelSize = ModelSize.Medium,
    TranscriptionEngine = TranscriptionEngine.Parakeet,
    ParakeetModelSize = ParakeetModelSize.Q4K,
    AutoPasteEnabled = false,
    TermsDictionary = "API,GPU,Клавиатура",
    SilenceThresholdMs = 900,
    StartWithWindows = true,
    StartMinimized = true,
    Theme = AppTheme.Dark,
    HideOnFocusLoss = true,
    AppLanguage = AppLanguage.En,
    NoiseReductionEnabled = true,
    Temperature = 0.7,
    ConditionOnPreviousText = true,
    MicrophoneDeviceId = "wasapi:{0.0.0.00000000}{6e8f2c11-4b2a-4c3d-9e55-0a1b2c3d4e5f}",
};

// Pre-engine file: no TranscriptionEngine / ParakeetModelSize, and one gamepad field null
// (that field did not exist in even older files).
var legacy = new LegacyAppSettings
{
    RecordingMode = RecordingMode.Toggle,
    RecordHotkey = "Ctrl+Shift+F9",
    CancelHotkey = "Ctrl+Shift+F10",
    RecordGamepadButton = "DInput|Microsoft|0",
    CancelGamepadButton = null,
    Language = RecognitionLanguage.Auto,
    ModelSize = ModelSize.Large,
    AutoPasteEnabled = true,
    TermsDictionary = "CPU,GPU",
    SilenceThresholdMs = 1500,
    StartWithWindows = false,
    StartMinimized = true,
    Theme = AppTheme.Light,
    HideOnFocusLoss = false,
    AppLanguage = AppLanguage.En,
    NoiseReductionEnabled = true,
    Temperature = 0.0,
    ConditionOnPreviousText = false,
    MicrophoneDeviceId = null,
};

var pascal = JsonNode.Parse(JsonSerializer.Serialize(allFields, pascalOptions))!.AsObject();
pascal["UnknownScalar"] = 42;
pascal["futureFlag"] = true;
pascal["UnknownObject"] = new JsonObject
{
    ["nested"] = new JsonArray(1, 2, 3),
    ["label"] = "not-a-real-setting",
};

// Numeric enum values next to one invalid enum string. Every non-enum field is non-default so
// that "the whole document fell back to defaults" is an observable outcome.
var invalid = JsonNode.Parse(JsonSerializer.Serialize(defaults, options))!.AsObject();
invalid["recordingMode"] = 1;              // numeric, in range -> Toggle
invalid["recordHotkey"] = "F9";
invalid["cancelHotkey"] = "F10";
invalid["recordGamepadButton"] = "XInput|B";
invalid["cancelGamepadButton"] = "XInput|Y";
invalid["language"] = "auto";
invalid["modelSize"] = "colossal";         // invalid enum string -> JsonException -> defaults
invalid["transcriptionEngine"] = 1;        // numeric, in range -> Parakeet
invalid["parakeetModelSize"] = 2;          // numeric, in range -> Q6K
invalid["autoPasteEnabled"] = false;
invalid["termsDictionary"] = "ZZZ";
invalid["silenceThresholdMs"] = 500;
invalid["startWithWindows"] = true;
invalid["startMinimized"] = true;
invalid["theme"] = "light";
invalid["hideOnFocusLoss"] = true;
invalid["appLanguage"] = "en";
invalid["noiseReductionEnabled"] = true;
invalid["temperature"] = 0.25;
invalid["conditionOnPreviousText"] = true;
invalid["microphoneDeviceId"] = "fixture-device";

var documents = new (string Name, string Text)[]
{
    ("settings-defaults", JsonSerializer.Serialize(defaults, options)),
    ("settings-all-fields", JsonSerializer.Serialize(allFields, options)),
    ("settings-legacy-missing-engine", JsonSerializer.Serialize(legacy, options)),
    ("settings-pascal-unknown", JsonSerializer.Serialize(pascal, options)),
    ("settings-invalid-and-numeric-enums", JsonSerializer.Serialize(invalid, options)),
};

var utf8NoBom = new UTF8Encoding(encoderShouldEmitUTF8Identifier: false);
var scratchDirectory = Path.Combine(Path.GetTempPath(), "vt-fixture-verify", Guid.NewGuid().ToString("N"));
Directory.CreateDirectory(scratchDirectory);

Console.WriteLine($"runtime={RuntimeInformation.FrameworkDescription} os={RuntimeInformation.OSDescription}");
Console.WriteLine($"newline={(Environment.NewLine == "\r\n" ? "CRLF" : "LF")} legacySchema={string.Join(",", legacySchema)}");

try
{
    foreach (var (name, text) in documents)
    {
        var path = Path.Combine(outputDirectory, name + ".json");
        File.WriteAllText(path, text, utf8NoBom);

        var bytes = File.ReadAllBytes(path);
        var sha = Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();
        var crlf = CountOccurrences(text, "\r\n");
        var lf = CountOccurrences(text, "\n");
        var bom = bytes.Length >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF;

        Console.WriteLine();
        Console.WriteLine($"=== {name}.json bytes={bytes.Length} sha256={sha} crlf={crlf} lf={lf} bom={bom} trailingNewline={text.EndsWith("\n", StringComparison.Ordinal)}");

        // Realistic load path: the file is what SettingsService.Load() would read.
        var loadDirectory = Path.Combine(scratchDirectory, name);
        Directory.CreateDirectory(loadDirectory);
        File.Copy(path, Path.Combine(loadDirectory, "settings.json"), overwrite: true);

        AppSettings? loaded;
        try
        {
            loaded = new SettingsService(loadDirectory).Load();
            Console.WriteLine("  rawDeserialize=OK");
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  rawDeserialize=THROW {ex.GetType().FullName}: {ex.Message}");
            continue;
        }

        var viaService = new SettingsService(loadDirectory).Load();
        Console.WriteLine($"  rawDeserializeOk={loaded is not null} serviceLoadEqualsDefaults={Defaults(viaService)}");
        Console.WriteLine($"  loaded={Dump(viaService)}");
    }
}
finally
{
    Directory.Delete(scratchDirectory, recursive: true);
}

return 0;

static int CountOccurrences(string haystack, string needle) =>
    haystack.Split(needle).Length - 1;

static bool Defaults(AppSettings s) =>
    s.RecordingMode == RecordingMode.PushToTalk
    && s.RecordHotkey == "Ctrl+Alt+Space"
    && s.CancelHotkey == "Ctrl+Alt+Escape"
    && s.RecordGamepadButton is null
    && s.CancelGamepadButton is null
    && s.Language == RecognitionLanguage.Ru
    && s.ModelSize == ModelSize.Small
    && s.TranscriptionEngine == TranscriptionEngine.Whisper
    && s.ParakeetModelSize == ParakeetModelSize.Q8_0
    && s.AutoPasteEnabled
    && s.TermsDictionary == "API,CPU,GPU,ASR,STT,TTS,LLM,JSON,IDE,SQL"
    && s.SilenceThresholdMs == 1200
    && !s.StartWithWindows
    && !s.StartMinimized
    && s.Theme == AppTheme.System
    && !s.HideOnFocusLoss
    && s.AppLanguage == AppLanguage.Ru
    && !s.NoiseReductionEnabled
    && s.Temperature == 0.0
    && !s.ConditionOnPreviousText
    && s.MicrophoneDeviceId is null;

static string Text(string? value) => value is null ? "null" : "\"" + value + "\"";

static string Dump(AppSettings s) => string.Join(
    " ",
    $"recordingMode={s.RecordingMode}",
    $"recordHotkey={Text(s.RecordHotkey)}",
    $"cancelHotkey={Text(s.CancelHotkey)}",
    $"recordGamepadButton={Text(s.RecordGamepadButton)}",
    $"cancelGamepadButton={Text(s.CancelGamepadButton)}",
    $"language={s.Language}",
    $"modelSize={s.ModelSize}",
    $"transcriptionEngine={s.TranscriptionEngine}",
    $"parakeetModelSize={s.ParakeetModelSize}",
    $"autoPasteEnabled={s.AutoPasteEnabled}",
    $"termsDictionary={Text(s.TermsDictionary)}",
    $"silenceThresholdMs={s.SilenceThresholdMs.ToString(CultureInfo.InvariantCulture)}",
    $"startWithWindows={s.StartWithWindows}",
    $"startMinimized={s.StartMinimized}",
    $"theme={s.Theme}",
    $"hideOnFocusLoss={s.HideOnFocusLoss}",
    $"appLanguage={s.AppLanguage}",
    $"noiseReductionEnabled={s.NoiseReductionEnabled}",
    $"temperature={s.Temperature.ToString("R", CultureInfo.InvariantCulture)}",
    $"conditionOnPreviousText={s.ConditionOnPreviousText}",
    $"microphoneDeviceId={Text(s.MicrophoneDeviceId)}");

/// <summary>AppSettings as it was written before commit 1163045 (no engine fields).</summary>
internal sealed class LegacyAppSettings
{
    public RecordingMode RecordingMode { get; set; } = RecordingMode.PushToTalk;

    public string RecordHotkey { get; set; } = "Ctrl+Alt+Space";

    public string CancelHotkey { get; set; } = "Ctrl+Alt+Escape";

    public string? RecordGamepadButton { get; set; }

    public string? CancelGamepadButton { get; set; }

    public RecognitionLanguage Language { get; set; } = RecognitionLanguage.Ru;

    public ModelSize ModelSize { get; set; } = ModelSize.Small;

    public bool AutoPasteEnabled { get; set; } = true;

    public string TermsDictionary { get; set; } = "API,CPU,GPU,ASR,STT,TTS,LLM,JSON,IDE,SQL";

    public int SilenceThresholdMs { get; set; } = 1200;

    public bool StartWithWindows { get; set; }

    public bool StartMinimized { get; set; }

    public AppTheme Theme { get; set; } = AppTheme.System;

    public bool HideOnFocusLoss { get; set; }

    public AppLanguage AppLanguage { get; set; } = AppLanguage.Ru;

    public bool NoiseReductionEnabled { get; set; }

    public double Temperature { get; set; } = 0.0;

    public bool ConditionOnPreviousText { get; set; }

    public string? MicrophoneDeviceId { get; set; }
}
