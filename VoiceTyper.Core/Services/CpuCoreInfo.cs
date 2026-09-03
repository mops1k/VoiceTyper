using System.Runtime.InteropServices;

namespace VoiceTyper.Core.Services;

/// <summary>
/// Определение числа физических ядер CPU.
/// Для матричных операций (энкодер Whisper) whisper.cpp эффективнее при потоков,
/// равном числу физических ядер, а не логических (гипертрединг/SMT даёт минус).
/// </summary>
public static class CpuCoreInfo
{
    /// <summary>Число физических ядер процессора; fallback — поровну от логических.</summary>
    public static int GetPhysicalCoreCount()
    {
        var count = InternalGetCount();
        return count > 0 ? count : Fallback();
    }

    private static int Fallback()
    {
        // На большинстве CPU примерно пополам физических/логических.
        return Math.Max(1, Environment.ProcessorCount / 2);
    }

    private static int InternalGetCount()
    {
        try
        {
            var size = Marshal.SizeOf<SYSTEM_LOGICAL_PROCESSOR_INFORMATION>();
            // Заводим буфер с запасом (обычно нужно ~число логических ядер * 2 структур).
            var capacity = Math.Max(Environment.ProcessorCount * 2, 4);
            using var buffer = new HGlobalBuffer((long)capacity * size);
            uint length = (uint)(capacity * size);
            if (!GetLogicalProcessorInformation(buffer.Pointer, ref length))
            {
                return 0;
            }

            var coreCount = 0;
            for (int offset = 0; offset + size <= length; offset += size)
            {
                var info = Marshal.PtrToStructure<SYSTEM_LOGICAL_PROCESSOR_INFORMATION>(IntPtr.Add(buffer.Pointer, offset));
                if (info.Relationship == RelationProcessorCore)
                {
                    coreCount++;
                }
            }

            return coreCount;
        }
        catch
        {
            return 0;
        }
    }

    private const int RelationProcessorCore = 0;

    [StructLayout(LayoutKind.Sequential)]
    private struct SYSTEM_LOGICAL_PROCESSOR_INFORMATION
    {
        public UIntPtr ProcessorMask;
        public int Relationship;
        public byte Reserved0;
        public byte Reserved1;
        public byte Reserved2;
        public byte Reserved3;
        public byte Reserved4;
        public byte Reserved5;
        public byte Reserved6;
        public byte Reserved7;
        public byte Reserved8;
        public byte Reserved9;
        public byte Reserved10;
        public byte Reserved11;
    }

    private sealed class HGlobalBuffer : IDisposable
    {
        public IntPtr Pointer { get; }

        public HGlobalBuffer(long size)
        {
            Pointer = Marshal.AllocHGlobal((int)size);
        }

        public void Dispose()
        {
            if (Pointer != IntPtr.Zero)
            {
                Marshal.FreeHGlobal(Pointer);
            }
        }
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetLogicalProcessorInformation(IntPtr buffer, ref uint length);
}
