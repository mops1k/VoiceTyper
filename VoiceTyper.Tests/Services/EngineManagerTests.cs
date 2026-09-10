using System.Runtime.InteropServices;
using VoiceTyper.Core.Interop;
using VoiceTyper.Core.Models;
using VoiceTyper.Core.Services;
using VoiceTyper.Core.Services.Transcription;

namespace VoiceTyper.Tests.Services;

public class EngineManagerTests
{
    [Fact]
    public void IsAvailable_WhisperAlwaysAvailable()
    {
        var manager = new EngineManager();

        Assert.True(manager.IsAvailable(TranscriptionEngine.Whisper));
    }

    [Fact]
    public void IsAvailable_ParakeetDependsOnDllPresence()
    {
        var manager = new EngineManager();
        var expected = File.Exists(Path.Combine(AppContext.BaseDirectory, "parakeet.dll"));

        Assert.Equal(expected, manager.IsAvailable(TranscriptionEngine.Parakeet));
    }

    [Fact]
    public void Create_Whisper_BuildsWhisperEngine()
    {
        var manager = new EngineManager();

        Assert.IsType<WhisperEngine>(manager.Create(TranscriptionEngine.Whisper, "some-model.bin"));
    }

    [Fact]
    public void Create_Parakeet_BuildsParakeetEngine()
    {
        // parakeet.dll копируется в output тестового проекта (Native\*.dll),
        // поэтому Create обязан работать без исключений.
        if (File.Exists(Path.Combine(AppContext.BaseDirectory, "parakeet.dll")))
        {
            var manager = new EngineManager();

            Assert.IsType<ParakeetEngine>(manager.Create(TranscriptionEngine.Parakeet, "some-model.gguf"));
        }
    }
}

/// <summary>Интеграционные тесты нативного C-API parakeet.cpp (пропускаются, если parakeet.dll нет рядом).</summary>
public class ParakeetNativeTests
{
    private static readonly string NativeDllPath =
        Path.Combine(AppContext.BaseDirectory, "parakeet.dll");

    private static bool DllPresent() => File.Exists(NativeDllPath);

    [Fact]
    public void AbiVersion_ReturnsPositiveNumber()
    {
        if (!DllPresent())
        {
            return; // Не проверяется в окружении без parakeet.dll.
        }

        var version = ParakeetNative.AbiVersionNumber();
        Assert.True(version > 0, $"ABI version {version} должен быть > 0");
    }

    [Fact]
    public void Load_MissingModel_ReturnsNullCtx()
    {
        if (!DllPresent())
        {
            return;
        }

        var ctx = ParakeetNative.Load(Path.Combine(NativeDllPath, "..", "__missing__.gguf"));

        // Спецификация C-API: при неудаче load() вернёт NULL. Для NULL-контекста
        // last_error обязан вернуть "" (ошибка недоступна, но безопасно читаема).
        Assert.Equal(IntPtr.Zero, ctx);
        Assert.Equal("", ParakeetNative.LastErrorText(ctx));

        // free(NULL) не должен падать.
        ParakeetNative.Free(ctx);
    }

    [Fact]
    public void Free_OnNull_DoesNotThrow()
    {
        if (!DllPresent())
        {
            return;
        }

        ParakeetNative.Free(IntPtr.Zero);
    }
}
