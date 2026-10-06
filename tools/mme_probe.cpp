// Scratch probe: can the MME (winmm waveIn) backend capture audio on this machine?
//
// The .NET chain ends with MME (compatibility-contracts.md: mc_wasapi.dll -> Raw
// WASAPI -> NAudio WASAPI -> MME), and on the target machine both our WASAPI path and
// mc_wasapi.dll open the device but deliver nothing. Before building an MME path into
// the application, this probe answers whether waveIn can hear anything at all.

#include <windows.h>
#include <mmsystem.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

constexpr int kSampleRate = 48000;
constexpr int kChannels = 2;
constexpr int kSeconds = 3;

std::atomic<int> g_buffers{0};
std::atomic<std::size_t> g_bytes{0};
std::atomic<double> g_peak{0.0};

void CALLBACK wave_in_proc(HWAVEIN, UINT message, DWORD_PTR instance, DWORD_PTR param1, DWORD_PTR param2)
{
    if (message != WIM_DATA) {
        return;
    }
    auto* header = reinterpret_cast<WAVEHDR*>(param1);
    if (header == nullptr || header->dwBytesRecorded == 0) {
        ::waveInAddBuffer(reinterpret_cast<HWAVEIN>(instance), header, sizeof(WAVEHDR));
        return;
    }
    g_buffers.fetch_add(1);
    g_bytes.fetch_add(header->dwBytesRecorded);
    const auto* samples = reinterpret_cast<const std::int16_t*>(header->lpData);
    const std::size_t count = header->dwBytesRecorded / sizeof(std::int16_t);
    double peak = g_peak.load();
    for (std::size_t index = 0; index < count; ++index) {
        const double value = std::abs(static_cast<double>(samples[index]) / 32768.0);
        if (value > peak) {
            peak = value;
        }
    }
    g_peak.store(peak);
    header->dwBytesRecorded = 0;
    ::waveInAddBuffer(reinterpret_cast<HWAVEIN>(instance), header, sizeof(WAVEHDR));
    static_cast<void>(param2);
}

} // namespace

int main()
{
    const UINT devices = ::waveInGetNumDevs();
    std::printf("mme-probe: %u waveIn device(s)\n", devices);
    for (UINT index = 0; index < devices; ++index) {
        WAVEINCAPSW caps {};
        if (::waveInGetDevCapsW(index, &caps, sizeof(caps)) == MMSYSERR_NOERROR) {
            char name[256] = {};
            ::WideCharToMultiByte(CP_UTF8, 0, caps.szPname, -1, name, sizeof(name), nullptr, nullptr);
            std::printf("  [%u] %s\n", index, name);
        }
    }

    WAVEFORMATEX format {};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = kChannels;
    format.nSamplesPerSec = kSampleRate;
    format.wBitsPerSample = 16;
    format.nBlockAlign = static_cast<WORD>(format.nChannels * format.wBitsPerSample / 8);
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

    HWAVEIN handle = nullptr;
    MMRESULT result = ::waveInOpen(&handle, WAVE_MAPPER, &format, reinterpret_cast<DWORD_PTR>(&wave_in_proc), 0,
        CALLBACK_FUNCTION);
    if (result != MMSYSERR_NOERROR) {
        std::printf("mme-probe: waveInOpen failed with %u\n", static_cast<unsigned>(result));
        return 1;
    }

    constexpr int kBufferCount = 4;
    constexpr DWORD kBufferBytes = kSampleRate * kChannels * 2 / 10; // 100 ms
    std::vector<std::vector<std::uint8_t>> buffers(kBufferCount, std::vector<std::uint8_t>(kBufferBytes));
    std::vector<WAVEHDR> headers(kBufferCount);
    for (int index = 0; index < kBufferCount; ++index) {
        headers[index].lpData = reinterpret_cast<LPSTR>(buffers[index].data());
        headers[index].dwBufferLength = kBufferBytes;
        ::waveInPrepareHeader(handle, &headers[index], sizeof(WAVEHDR));
        ::waveInAddBuffer(handle, &headers[index], sizeof(WAVEHDR));
    }

    result = ::waveInStart(handle);
    if (result != MMSYSERR_NOERROR) {
        std::printf("mme-probe: waveInStart failed with %u\n", static_cast<unsigned>(result));
        return 1;
    }
    ::Sleep(static_cast<DWORD>(kSeconds) * 1000);
    ::waveInStop(handle);
    ::waveInReset(handle);
    for (auto& header : headers) {
        ::waveInUnprepareHeader(handle, &header, sizeof(WAVEHDR));
    }
    ::waveInClose(handle);

    std::printf("mme-probe: buffers=%d bytes=%zu peak=%.4f\n", g_buffers.load(), g_bytes.load(), g_peak.load());
    return g_bytes.load() == 0 ? 2 : 0;
}
