namespace VoiceTyper.Core.Models;

using VoiceTyper.Core.Services;

/// <summary>Режим запуска записи.</summary>
public enum RecordingMode
{
    /// <summary>Держишь горячую клавишу — идёт запись.</summary>
    PushToTalk,

    /// <summary>Нажал — начал, нажал ещё раз — остановил.</summary>
    Toggle,

    /// <summary>Автоматическая остановка по тишине (VAD).</summary>
    Vad,
}

/// <summary>Тема внешнего вида приложения.</summary>
public enum AppTheme
{
    /// <summary>Следовать теме Windows (системная).</summary>
    System,

    /// <summary>Всегда светлая.</summary>
    Light,

    /// <summary>Всегда тёмная.</summary>
    Dark,
}

/// <summary>Размер модели Whisper (ggml).</summary>
public enum ModelSize
{
    Tiny,
    Base,
    Small,
    Medium,
    Large,
}

/// <summary>Язык распознавания.</summary>
public enum RecognitionLanguage
{
    Auto,
    Ru,
    En,
}

/// <summary>Язык интерфейса приложения.</summary>
public enum AppLanguage
{
    Ru,
    En,
}

/// <summary>
/// Модель настроек приложения. Сериализуется в <c>%APPDATA%\VoiceTyper\settings.json</c>.
/// Enum'ы сериализуются в camelCase: <c>pushToTalk</c>, <c>ru</c>, <c>small</c>.
/// </summary>
public sealed class AppSettings
{
    public RecordingMode RecordingMode { get; set; } = RecordingMode.PushToTalk;

    /// <summary>Горячая клавиша запуска/остановки записи, например <c>Ctrl+Alt+Space</c>.</summary>
    public string RecordHotkey { get; set; } = "Ctrl+Alt+Space";

    /// <summary>Горячая клавиша отмены записи/обработки.</summary>
    public string CancelHotkey { get; set; } = "Ctrl+Alt+Escape";

    public RecognitionLanguage Language { get; set; } = RecognitionLanguage.Ru;

    public ModelSize ModelSize { get; set; } = ModelSize.Small;

    /// <summary>Автоматически вставлять текст (Ctrl+V) после распознавания. По умолчанию — вкл.</summary>
    public bool AutoPasteEnabled { get; set; } = true;

    /// <summary>Технические термины (через запятую), добавляются в initial prompt модели.</summary>
    public string TermsDictionary { get; set; } = "API,CPU,GPU,ASR,STT,TTS,LLM,JSON,IDE,SQL";

    /// <summary>Порог тишины в мс для режима VAD.</summary>
    public int SilenceThresholdMs { get; set; } = 1200;

    /// <summary>Автозапуск вместе с Windows.</summary>
    public bool StartWithWindows { get; set; }

    /// <summary>Старт свёрнутым в трей (по умолчанию — обычным окном).</summary>
    public bool StartMinimized { get; set; }

    /// <summary>Тема внешнего вида (по умолчанию — системная).</summary>
    public AppTheme Theme { get; set; } = AppTheme.System;

    /// <summary>Сворачивать окно настроек в трей при потере фокуса (по умолчанию — выкл).</summary>
    public bool HideOnFocusLoss { get; set; }

    /// <summary>Язык интерфейса приложения (по умолчанию — русский).</summary>
    public AppLanguage AppLanguage { get; set; } = AppLanguage.Ru;

    /// <summary>Включать подавление фонового шума при записи.</summary>
    public bool NoiseReductionEnabled { get; set; }

    /// <summary>Температура распознавания (0 = строго/детерминированно, выше — мягче).</summary>
    public double Temperature { get; set; } = 0.0;

    /// <summary>Использовать текст предыдущего сегмента как контекст (для длинной речи).</summary>
    public bool ConditionOnPreviousText { get; set; }

    /// <summary>ID микрофона (пустой — устройство по умолчанию).</summary>
    public string? MicrophoneDeviceId { get; set; }

    public AppSettings Clone()
    {
        var json = System.Text.Json.JsonSerializer.Serialize(this, SettingsService.JsonOptions);
        return System.Text.Json.JsonSerializer.Deserialize<AppSettings>(json, SettingsService.JsonOptions)!;
    }
}
