using System.Runtime.InteropServices;

namespace VoiceTyper.Core.Interop;

/// <summary>
/// Плоский P/Invoke-слой поверх C-API <c>parakeet_capi_*</c> нативного рантайма parakeet.cpp
/// (parakeet.dll рядом с приложением; Cdecl-конвенция, версия ABI проверяется вызовом
/// <see cref="AbiVersion"/>). Возвращённые строки — malloc'd UTF-8, освобождаются
/// <see cref="FreeString"/>; <see cref="LastError"/> живёт в контексте и НЕ освобождается.
/// </summary>
internal static class ParakeetNative
{
    private const string LibName = "parakeet";

    [DllImport(LibName, EntryPoint = "parakeet_capi_abi_version", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    private static extern int parakeet_capi_abi_version();

    [DllImport(LibName, EntryPoint = "parakeet_capi_load", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    private static extern IntPtr parakeet_capi_load([MarshalAs(UnmanagedType.LPUTF8Str)] string ggufPath);

    [DllImport(LibName, EntryPoint = "parakeet_capi_free", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    private static extern void parakeet_capi_free(IntPtr ctx);

    [DllImport(LibName, EntryPoint = "parakeet_capi_transcribe_pcm", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    private static extern IntPtr parakeet_capi_transcribe_pcm(IntPtr ctx, float[] samples, int nSamples, int sampleRate, int decoder);

    [DllImport(LibName, EntryPoint = "parakeet_capi_transcribe_pcm_lang", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    private static extern IntPtr parakeet_capi_transcribe_pcm_lang(IntPtr ctx, float[] samples, int nSamples, int sampleRate, int decoder,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string targetLang);

    [DllImport(LibName, EntryPoint = "parakeet_capi_free_string", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    private static extern void parakeet_capi_free_string(IntPtr s);

    [DllImport(LibName, EntryPoint = "parakeet_capi_last_error", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    private static extern IntPtr parakeet_capi_last_error(IntPtr ctx);

    /// <summary>Версия ABI нативной библиотеки (соответствует заголовку parakeet_capi.h).</summary>
    internal static int AbiVersionNumber() => parakeet_capi_abi_version();

    /// <summary>Загружает GGUF-модель. Возвращает непрозрачный контекст или IntPtr.Zero при ошибке.</summary>
    internal static IntPtr Load(string ggufPath) => parakeet_capi_load(ggufPath);

    /// <summary>Освобождает контекст. Безопасно для IntPtr.Zero.</summary>
    internal static void Free(IntPtr ctx) => parakeet_capi_free(ctx);

    /// <summary>
    /// Транскрибирует моно float PCM (linearly resampled, если не 16 кГц). decoder: 0 = по умолчанию
    /// (TDT для v3). Возвращает malloc'd UTF-8 строку (освободить <see cref="FreeString"/>) или
    /// IntPtr.Zero при ошибке (см. <see cref="LastError"/>).
    /// </summary>
    internal static IntPtr TranscribePcm(IntPtr ctx, float[] samples, int nSamples, int sampleRate, int decoder)
        => parakeet_capi_transcribe_pcm(ctx, samples, nSamples, sampleRate, decoder);

    /// <summary>
    /// Как <see cref="TranscribePcm"/>, но с явным целевым языком для мультиязычных
    /// (prompt-conditioned, "nemotron") моделей; "" или null — язык модели (auto).
    /// </summary>
    internal static IntPtr TranscribePcmLang(IntPtr ctx, float[] samples, int nSamples, int sampleRate, int decoder, string? targetLang)
        => targetLang is null or ""
            ? parakeet_capi_transcribe_pcm(ctx, samples, nSamples, sampleRate, decoder)
            : parakeet_capi_transcribe_pcm_lang(ctx, samples, nSamples, sampleRate, decoder, targetLang);

    /// <summary>Освобождает malloc'd строку, возвращённую transcribe_* (Безопасно для IntPtr.Zero).</summary>
    internal static void FreeString(IntPtr s) => parakeet_capi_free_string(s);

    /// <summary>Человекочитаемое описание последней ошибки контекста (или "" если её нет). НЕ освобождать.</summary>
    internal static string LastErrorText(IntPtr ctx)
        => Marshal.PtrToStringUTF8(parakeet_capi_last_error(ctx)) ?? "";
}
