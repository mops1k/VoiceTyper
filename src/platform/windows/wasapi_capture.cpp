// WASAPI capture: the only file in the Windows backend that names COM audio
// interfaces. Everything portable sees wasapi_capture.hpp, which has no
// <windows.h> and no COM type in a single signature.
//
// Contract evidence is in wasapi_capture.hpp; the short version:
//   * every COM object is owned by a ComPtr and released on every path, so a
//     refused candidate cannot leak the IAudioClient it activated (the defect
//     recorded for the .NET RawWasapiCapture in m_00379ac352a2);
//   * no HRESULT ever escapes a call boundary: each COM call is checked and
//     turned into a Status plus a diagnostic line, because the point of the
//     instrumented candidate walk is that a machine-specific refusal is
//     explainable instead of a bare "could not open the microphone";
//   * the capture thread is the only thread that touches IAudioClient, and
//     stop() joins it before the sink is dropped, so the .NET ordering
//     (device -> unsubscribe -> snapshot, and never clear before the snapshot)
//     is structurally guaranteed rather than merely intended.

#include "platform/windows/wasapi_capture.hpp"

#if !defined(_WIN32)
#error "wasapi_capture.cpp is Windows platform code; the target must be WIN32-only"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <objbase.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace voicetyper::platform {
namespace {

// ---------------------------------------------------------------------------
// COM plumbing
// ---------------------------------------------------------------------------

/// Initializes COM for the calling thread and balances it on scope exit.
///
/// RPC_E_CHANGED_MODE means the thread is already in a single-threaded
/// apartment because somebody else got there first. COM is still usable there
/// (nothing in this backend needs a free-threaded apartment) but this object
/// must not uninitialize it, so `owned_` stays false.
class ComScope {
public:
    ComScope() noexcept
    {
        const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        result_ = hr;
        if (hr == S_OK) {
            // Only our own initialisation is balanced by CoUninitialize: an
            // S_FALSE answer means the thread was already MTA and this call did
            // not take a reference.
            owned_ = true;
            available_ = true;
        } else if (hr == S_FALSE) {
            owned_ = false;
            available_ = true;
        } else if (hr == RPC_E_CHANGED_MODE) {
            owned_ = false;
            available_ = true;
        } else {
            available_ = false;
            failure_ = hr;
        }
    }

    ~ComScope()
    {
        if (owned_) {
            ::CoUninitialize();
        }
    }

    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;

    [[nodiscard]] bool available() const noexcept { return available_; }
    [[nodiscard]] HRESULT failure() const noexcept { return failure_; }

    /// The raw CoInitializeEx result: S_OK (this call initialised the thread as
    /// MTA), S_FALSE (the thread was already MTA), RPC_E_CHANGED_MODE (the
    /// thread is in another apartment - we join it and never uninitialise), or a
    /// failure. It matters because IAudioClient in exclusive mode is documented
    /// to require an MTA thread.
    [[nodiscard]] HRESULT result() const noexcept { return result_; }

private:
    bool owned_ = false;
    bool available_ = false;
    HRESULT failure_ = S_OK;
    HRESULT result_ = S_OK;
};

/// Minimal owning COM pointer. Release() runs in the destructor, so every
/// early return in this file releases whatever it holds.
template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { reset(); }

    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    ComPtr(ComPtr&& other) noexcept
        : ptr_(other.ptr_)
    {
        other.ptr_ = nullptr;
    }

    ComPtr& operator=(ComPtr&& other) noexcept
    {
        if (this != &other) {
            reset();
            ptr_ = other.ptr_;
            other.ptr_ = nullptr;
        }
        return *this;
    }

    /// Address for a Create/QueryInterface-style out parameter. The previous
    /// value is released first, so handing this to a COM call is safe.
    [[nodiscard]] T** put() noexcept
    {
        reset();
        return &ptr_;
    }

    void reset() noexcept
    {
        if (ptr_ != nullptr) {
            ptr_->Release();
            ptr_ = nullptr;
        }
    }

    [[nodiscard]] T* get() const noexcept { return ptr_; }
    [[nodiscard]] T* operator->() const noexcept { return ptr_; }
    [[nodiscard]] explicit operator bool() const noexcept { return ptr_ != nullptr; }

    friend bool operator==(const ComPtr& ptr, std::nullptr_t) noexcept { return ptr.ptr_ == nullptr; }
    friend bool operator!=(const ComPtr& ptr, std::nullptr_t) noexcept { return ptr.ptr_ != nullptr; }
    friend bool operator==(std::nullptr_t, const ComPtr& ptr) noexcept { return ptr.ptr_ == nullptr; }
    friend bool operator!=(std::nullptr_t, const ComPtr& ptr) noexcept { return ptr.ptr_ != nullptr; }

private:
    T* ptr_ = nullptr;
};

/// RAII for the WAVEFORMATEX that IAudioClient::GetMixFormat allocates with
/// CoTaskMemAlloc. It has to live for the whole candidate attempt: the pointer
/// is what IAudioClient::Initialize is given, and the negotiated format is read
/// back from the same allocation.
struct MixFormatDeleter {
    void operator()(WAVEFORMATEX* format) const noexcept { ::CoTaskMemFree(format); }
};
using MixFormatPtr = std::unique_ptr<WAVEFORMATEX, MixFormatDeleter>;

// ---------------------------------------------------------------------------
// HRESULT -> ErrorCode / text
// ---------------------------------------------------------------------------

std::string apartment_name(HRESULT result)
{
    switch (static_cast<unsigned long>(static_cast<DWORD>(result))) {
    case 0x00000000u: return "mta-owned";
    case 0x00000001u: return "mta-joined";
    case 0x80010106u: return "other-apartment(sta)";
    default: return "failed";
    }
}

std::string hex32(HRESULT hr)
{
    static const char* kDigits = "0123456789ABCDEF";
    std::string text = "0x";
    const auto value = static_cast<unsigned long>(static_cast<DWORD>(hr));
    for (int shift = 28; shift >= 0; shift -= 4) {
        text.push_back(kDigits[(value >> shift) & 0xFu]);
    }
    return text;
}

/// Maps a COM failure onto the portable error vocabulary. The switch covers the
/// failures this backend can actually observe, and the default is io_failure
/// rather than "unknown", because a status with a documented code is always
/// better than one that leaks a raw HRESULT into the UI.
/// COM status codes used in the HRESULT mapping below. MinGW-w64's headers do
/// not declare the whole error family in every configuration, so the values
/// that matter are named here instead of being silently dropped.
constexpr HRESULT kClassNotAvailable = static_cast<HRESULT>(0x80040154u); // CLASS_E_CLASSNOTAVAILABLE / REGDB_E_CLASSNOTREG
constexpr HRESULT kRpcServerUnavailable = static_cast<HRESULT>(0x80040155u);
constexpr HRESULT kRpcServerDied = static_cast<HRESULT>(0x8001001Fu);

ErrorCode error_code_from_hresult(HRESULT hr) noexcept
{
    if (SUCCEEDED(hr)) {
        return ErrorCode::ok;
    }
    switch (static_cast<DWORD>(hr)) {
    case static_cast<DWORD>(HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED)):
        return ErrorCode::permission_denied;
    case static_cast<DWORD>(HRESULT_FROM_WIN32(ERROR_NOT_FOUND)):
    case static_cast<DWORD>(HRESULT_FROM_WIN32(ERROR_DEV_NOT_EXIST)):
        return ErrorCode::not_found;
    case static_cast<DWORD>(HRESULT_FROM_WIN32(ERROR_DEVICE_REMOVED)):
    case static_cast<DWORD>(AUDCLNT_E_DEVICE_INVALIDATED):
    case static_cast<DWORD>(AUDCLNT_E_RESOURCES_INVALIDATED):
        return ErrorCode::device_disconnected;
    case static_cast<DWORD>(AUDCLNT_E_SERVICE_NOT_RUNNING):
    case static_cast<DWORD>(HRESULT_FROM_WIN32(ERROR_BUSY)):
    case static_cast<DWORD>(AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED):
    case static_cast<DWORD>(AUDCLNT_E_EXCLUSIVE_MODE_ONLY):
    case static_cast<DWORD>(kRpcServerUnavailable):
    case static_cast<DWORD>(kRpcServerDied):
    case static_cast<DWORD>(CO_E_NOTINITIALIZED):
    case static_cast<DWORD>(kClassNotAvailable):
        return ErrorCode::unavailable;
    case static_cast<DWORD>(E_NOINTERFACE):
    case static_cast<DWORD>(E_NOTIMPL):
    case static_cast<DWORD>(AUDCLNT_E_UNSUPPORTED_FORMAT):
    case static_cast<DWORD>(AUDCLNT_E_INVALID_SIZE):
    case static_cast<DWORD>(HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED)):
    case static_cast<DWORD>(AUDCLNT_E_WRONG_ENDPOINT_TYPE):
        return ErrorCode::unsupported;
    case static_cast<DWORD>(E_OUTOFMEMORY):
    case static_cast<DWORD>(HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_MEMORY)):
        return ErrorCode::resource_exhausted;
    default:
        return ErrorCode::io_failure;
    }
}

Status failure_status(HRESULT hr, std::string message, std::string detail)
{
    ErrorCode code = error_code_from_hresult(hr);
    if (code == ErrorCode::ok) {
        // A Status::failure() that claims success would silently become a
        // successful start with no device, which is exactly the bug this
        // backend must not have.
        code = ErrorCode::io_failure;
    }
    return Status::failure(domain::Error(code, std::move(message), std::move(detail)));
}

/// One diagnostic line: "<what>: 0xHHHHHHHH". The .NET build logged the same
/// shape (`init=0x88890008`) into _rawDiagnostic, and it is what makes a device
/// refusal debuggable from a log file alone.
std::string diagnostic_line(std::string_view what, HRESULT hr)
{
    std::string line(what);
    line += ": ";
    line += hex32(hr);
    return line;
}

std::string narrow_utf8(const wchar_t* wide)
{
    if (wide == nullptr || *wide == L'\0') {
        return {};
    }
    const int needed = ::WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) {
        return {};
    }
    // `needed` includes the terminating NUL that -1 asks for; the string itself
    // is one character shorter.
    std::string out(static_cast<std::size_t>(needed), '\0');
    const int written = ::WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), needed, nullptr, nullptr);
    if (written <= 0) {
        return {};
    }
    out.resize(static_cast<std::size_t>(written - 1));
    return out;
}

/// The persisted settings value is UTF-8 while IMMDeviceEnumerator::GetDevice
/// takes the endpoint id as UTF-16. Empty means the stored value is unusable,
/// which the caller reports as invalid_argument rather than probing with a
/// half-converted id.
std::wstring widen_utf8(const std::string& text)
{
    if (text.empty()) {
        return {};
    }
    const int needed = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (needed <= 1) {
        return {};
    }
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    const int written = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, out.data(), needed);
    if (written <= 0) {
        return {};
    }
    out.resize(static_cast<std::size_t>(written - 1));
    return out;
}

// ---------------------------------------------------------------------------
// Format description
// ---------------------------------------------------------------------------

/// KSDATAFORMAT_SUBTYPE_PCM = {00000001-0000-0010-8000-00AA00389B71}. Declared
/// locally instead of pulling <ksmedia.h> into the audio include chain.
constexpr GUID kSubtypePcm = {
    0x00000001,
    0x0000,
    0x0010,
    { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 },
};

/// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT = {00000003-0000-0010-8000-00AA00389B71}.
constexpr GUID kSubtypeIeeeFloat = {
    0x00000003,
    0x0000,
    0x0010,
    { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 },
};

/// Channel mask for interleaved stereo (FL|FR).
constexpr std::uint32_t kStereoChannelMask = 0x3;

struct FormatDescription {
    domain::AudioFormat format;
    /// True when the endpoint will hand out PCM16 rather than IEEE float32.
    bool pcm16 = false;
    bool valid = false;
};

/// Interprets a WAVEFORMATEX the endpoint (or we) asked for, and rejects
/// anything the portable audio layer cannot consume. An 8-bit or 24-bit
/// endpoint is `unsupported` here rather than silently mangled later.
FormatDescription describe_format(const WAVEFORMATEX& wave)
{
    FormatDescription result;
    const auto channels = static_cast<std::uint16_t>(wave.nChannels);
    const auto rate = static_cast<std::uint32_t>(wave.nSamplesPerSec);
    if (channels == 0 || rate == 0 || channels > 8) {
        return result;
    }

    std::uint16_t tag = wave.wFormatTag;
    GUID subtype {};
    bool has_subtype = false;
    if (tag == WAVE_FORMAT_EXTENSIBLE) {
        if (wave.cbSize < static_cast<WORD>(sizeof(WAVEFORMATEXTENSIBLE))) {
            return result;
        }
        WAVEFORMATEXTENSIBLE extensible {};
        std::memcpy(&extensible, &wave, sizeof(extensible));
        subtype = extensible.SubFormat;
        has_subtype = true;
        tag = extensible.SubFormat.Data1;
    }

    if (tag == WAVE_FORMAT_PCM) {
        if (wave.wBitsPerSample != 16) {
            return result;
        }
        result.pcm16 = true;
    } else if (tag == WAVE_FORMAT_IEEE_FLOAT) {
        if (wave.wBitsPerSample != 32) {
            return result;
        }
        result.pcm16 = false;
    } else {
        return result;
    }

    if (has_subtype) {
        const GUID expected = result.pcm16 ? kSubtypePcm : kSubtypeIeeeFloat;
        if (std::memcmp(&subtype, &expected, sizeof(GUID)) != 0) {
            return result;
        }
    }

    result.format = domain::AudioFormat(
        rate,
        channels,
        result.pcm16 ? domain::SampleFormat::pcm_s16 : domain::SampleFormat::ieee_float32);
    result.valid = true;
    return result;
}

/// Shared-mode stream flags that let the audio engine convert between our PCM16
/// format and the endpoint's own format. MinGW-w64's audioclient.h at this
/// version does not declare them, so the documented values are named here rather
/// than silently omitted (the same approach the HRESULT table below uses).
constexpr DWORD kStreamFlagsAutoConvertPcm = 0x80000000u; // AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
constexpr DWORD kStreamFlagsSrcDefaultQuality = 0x08000000u; // AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY

WAVEFORMATEXTENSIBLE make_extensible_pcm16(std::uint32_t rate, std::uint16_t channels)
{
    WAVEFORMATEXTENSIBLE wave {};
    wave.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wave.Format.nChannels = channels;
    wave.Format.nSamplesPerSec = rate;
    wave.Format.wBitsPerSample = 16;
    wave.Format.nBlockAlign = static_cast<WORD>(channels * 2);
    wave.Format.nAvgBytesPerSec = rate * wave.Format.nBlockAlign;
    wave.Format.cbSize = static_cast<WORD>(sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX));
    wave.dwChannelMask = kStereoChannelMask;
    wave.SubFormat = kSubtypePcm;
    return wave;
}

WAVEFORMATEX make_pcm16(std::uint32_t rate, std::uint16_t channels)
{
    WAVEFORMATEX wave {};
    wave.wFormatTag = WAVE_FORMAT_PCM;
    wave.nChannels = channels;
    wave.nSamplesPerSec = rate;
    wave.wBitsPerSample = 16;
    wave.nBlockAlign = static_cast<WORD>(channels * 2);
    wave.nAvgBytesPerSec = rate * wave.nBlockAlign;
    wave.cbSize = 0;
    return wave;
}

// ---------------------------------------------------------------------------
// Candidate negotiation
// ---------------------------------------------------------------------------

/// One (share mode x format x stream mode x buffer size) combination, tried in
/// order until one is accepted. This is the instrumented enumeration of
/// RawWasapiCapture, narrowed to the combinations that have ever worked:
/// shared mix format (Chromium/cpal, event driven) and exclusive 48 kHz PCM16
/// (the Intel Smart Sound built-in array, polling).
struct Candidate {
    bool exclusive = false;
    /// True means "let the engine use its own mix format" (shared mode only).
    bool use_mix_format = false;
    bool extensible = false;
    bool event_driven = false;
    std::uint32_t buffer_milliseconds = 100;
    /// Shared mode with the engine asked to convert for us
    /// (AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | SRC_DEFAULT_QUALITY). This is the
    /// only way a shared stream may be opened with a format the engine does not
    /// itself use, and leaving the flag out is what makes this driver answer
    /// E_INVALIDARG to every plain shared attempt.
    bool auto_convert = false;
};

[[nodiscard]] std::string candidate_name(const Candidate& candidate)
{
    std::string name = candidate.exclusive ? "exclusive/" : "shared/";
    if (candidate.use_mix_format) {
        name += "mix";
    } else {
        name += candidate.extensible ? "pcm16-extensible" : "pcm16";
    }
    if (candidate.auto_convert) {
        name += "-autoconvert";
    }
    name += candidate.event_driven ? "/event" : "/poll";
    name += "/" + std::to_string(candidate.buffer_milliseconds) + "ms";
    return name;
}

[[nodiscard]] std::vector<Candidate> build_candidates(WasapiStrategy strategy)
{
    // The parameters mirror RawWasapiCapture.cs, which is the reference that
    // actually works on this machine's Intel Smart Sound array:
    //   * `audioClient.Initialize(1, 0, 1_000_000, 0, PCM16 48k/2ch, NULL)` for
    //     exclusive mode - 1_000_000 reference units are 100 ms, not 1000 ms, and
    //     the buffer size is what this driver checks (a 1 s buffer came back as
    //     CO_E_NOTINITIALIZED 0x800401F0);
    //   * plain WAVEFORMATEX PCM16 first, WAVEFORMATEXTENSIBLE second, exactly as
    //     `TryExclusive` builds them;
    //   * shared mode with the engine mix format, EVENTCALLBACK and NO other
    //     flags. `AUDCLNT_STREAMFLAGS_NOPERSIST` is dropped because the working
    //     reference does not set it and this driver answers E_INVALIDARG for
    //     shared initialisation the moment anything is off ("shared всегда даёт
    //     E_INVALIDARG", RawWasapiCapture.cs:76);
    //   * the four shared combinations from the reference table: 100 ms event,
    //     10 ms event, automatic buffer polling, 100 ms polling.
    const Candidate exclusive_pcm { true, false, false, false, 100 };
    const Candidate exclusive_extensible { true, false, true, false, 100 };
    const Candidate shared_mix_event_100 { false, true, false, true, 100 };
    const Candidate shared_mix_event_10 { false, true, false, true, 10 };
    const Candidate shared_mix_poll_auto { false, true, false, false, 0 };
    const Candidate shared_mix_poll_100 { false, true, false, false, 100 };
    const Candidate shared_pcm_poll_100 { false, false, false, false, 100 };
    // The engine converts for us, so these work with our own PCM16 48 kHz/2ch
    // format instead of requiring the endpoint's mix format.
    const Candidate shared_pcm_convert_100 { false, false, false, false, 100, true };
    const Candidate shared_pcm_convert_auto { false, false, false, false, 0, true };
    // Exclusive mode also requires a format the device itself supports: the
    // endpoint's own mix format is the safest candidate, and an exclusive
    // event-driven stream needs periodicity == buffer duration.
    const Candidate exclusive_mix { true, true, false, false, 100 };
    const Candidate exclusive_pcm_event { true, false, false, true, 100 };

    switch (strategy) {
    case WasapiStrategy::shared_then_exclusive:
        return { shared_pcm_convert_100, shared_pcm_convert_auto, shared_mix_event_100,
            shared_mix_event_10, shared_mix_poll_auto, shared_mix_poll_100, exclusive_pcm,
            exclusive_pcm_event, exclusive_mix, exclusive_extensible, shared_pcm_poll_100 };
    case WasapiStrategy::exclusive_then_shared:
        return { exclusive_pcm, exclusive_pcm_event, exclusive_mix, exclusive_extensible,
            shared_pcm_convert_100, shared_pcm_convert_auto, shared_mix_event_100,
            shared_mix_event_10, shared_mix_poll_auto, shared_mix_poll_100, shared_pcm_poll_100 };
    case WasapiStrategy::shared_only:
        return { shared_pcm_convert_100, shared_pcm_convert_auto, shared_mix_event_100,
            shared_mix_event_10, shared_mix_poll_auto, shared_mix_poll_100, shared_pcm_poll_100 };
    }
    return { shared_pcm_convert_100 };
}

// ---------------------------------------------------------------------------
// The backend
// ---------------------------------------------------------------------------

class WasapiCaptureImpl final : public WasapiCapture {
public:
    WasapiCaptureImpl() = default;

    ~WasapiCaptureImpl() override { destroy(); }

    Status start(const WasapiCaptureOptions& options, WasapiPacketSink sink, const CancellationToken& cancellation) override;

    Status stop() override;

    void destroy() noexcept override;

    [[nodiscard]] bool is_capturing() const noexcept override { return capturing_; }

    [[nodiscard]] domain::AudioFormat device_format() const noexcept override { return device_format_; }

    [[nodiscard]] WasapiEndReason end_reason() const noexcept override
    {
        return end_reason_.load(std::memory_order_acquire);
    }

    [[nodiscard]] WasapiCaptureReport report() const override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return report_;
    }

private:
    Status resolve_device(const WasapiCaptureOptions& options, std::string& diagnostics);
    bool try_candidate(
        const Candidate& candidate, const WasapiCaptureOptions& options, std::string& diagnostics, HRESULT& failure);
    void capture_loop() noexcept;
    void close_events() noexcept;
    void release_session() noexcept;
    void wait_for_stop() noexcept;
    /// Starts the already negotiated client again (no create, no Initialize).
    bool reuse_session(std::string& diagnostics);

    mutable std::mutex mutex_;

    // COM session; all released in release_session().
    ComPtr<IMMDevice> device_;
    ComPtr<IAudioClient> client_;
    ComPtr<IAudioCaptureClient> capture_client_;

    WasapiPacketSink sink_;
    domain::AudioFormat device_format_;
    WasapiCaptureReport report_;
    std::vector<std::byte> silence_;

    std::thread thread_;
    void* stop_event_ = nullptr;
    void* render_event_ = nullptr;

    std::atomic<WasapiEndReason> end_reason_{ WasapiEndReason::completed };
    std::atomic<bool> thread_finished_{ false };
    std::mutex finished_mutex_;
    std::condition_variable finished_cv_;

    std::chrono::milliseconds idle_wait_{ 3 };
    std::chrono::milliseconds stop_timeout_{ 2000 };
    std::uint32_t bytes_per_frame_ = 0;
    bool event_driven_ = false;
    bool capturing_ = false;
    CancellationToken cancellation_;

    // Reuse across dictations. create + Initialize + the candidate scan cost about
    // a second on this machine (measured: "capture start 1017 ms" on every
    // session, and a 3 s hold delivered only 1.98 s of audio), while Start() on an
    // already negotiated client is immediate.
    std::string requested_device_id_;
    std::string last_device_id_;
    std::optional<WasapiStrategy> negotiated_strategy_;
    int negotiated_candidate_ = -1;
    bool negotiated_exclusive_ = false;
    std::uint32_t negotiated_buffer_ms_ = 0;
};

Status WasapiCaptureImpl::resolve_device(const WasapiCaptureOptions& options, std::string& diagnostics)
{
    ComScope com;
    if (!com.available()) {
        return failure_status(
            com.failure(), "COM is not available on this thread", diagnostic_line("coinitialize", com.failure()));
    }

    ComPtr<IMMDeviceEnumerator> enumerator;
    // The class id is the coclass CLSID, not the interface IID: passing
    // __uuidof(IMMDeviceEnumerator) there returns REGDB_E_CLASSNOTREG
    // (0x80040154) and was misread as "the Windows audio service is not
    // available". The interface IID belongs in the fourth argument.
    HRESULT hr = ::CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        nullptr,
        CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator),
        reinterpret_cast<void**>(enumerator.put()));
    if (FAILED(hr)) {
        diagnostics += diagnostic_line("mmdevice-enumerator", hr) + "; ";
        return failure_status(hr, "the Windows audio service is not available", diagnostic_line("cocreate", hr));
    }

    if (!options.device_id.empty()) {
        const std::wstring wide_id = widen_utf8(options.device_id);
        if (wide_id.empty()) {
            return failure_status(
                E_INVALIDARG,
                "the selected microphone id is not valid UTF-8",
                options.device_id);
        }
        hr = enumerator->GetDevice(wide_id.c_str(), device_.put());
        if (FAILED(hr)) {
            diagnostics += diagnostic_line("get-device", hr) + "; ";
            return failure_status(hr, "the selected microphone is not available", diagnostic_line("get-device", hr));
        }
        return Status::success();
    }

    // Default endpoint: eConsole first, eCommunications second, exactly the role
    // loop of RawWasapiCapture.TryRole. Windows can legitimately have a console
    // role without a communications one.
    const ERole roles[] = { eConsole, eCommunications };
    HRESULT last = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    for (ERole role : roles) {
        hr = enumerator->GetDefaultAudioEndpoint(eCapture, role, device_.put());
        if (SUCCEEDED(hr)) {
            return Status::success();
        }
        last = hr;
        diagnostics += diagnostic_line(role == eConsole ? "default-console" : "default-communications", hr) + "; ";
    }
    return failure_status(last, "no default recording endpoint", diagnostic_line("default", last));
}

bool WasapiCaptureImpl::try_candidate(
    const Candidate& candidate, const WasapiCaptureOptions& options, std::string& diagnostics, HRESULT& failure)
{
    const std::string label = candidate_name(candidate);

    ComPtr<IAudioClient> client;
    HRESULT hr = device_->Activate(
        __uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.put()));
    if (FAILED(hr)) {
        failure = hr;
        diagnostics += diagnostic_line(label + " activate", hr) + "; ";
        return false;
    }

    // The client properties are deliberately NOT set. An earlier attempt followed
    // Chromium and asked for AudioCategory_Other before initialising, on the theory
    // that the Intel Smart Sound array needs to be told the stream category; on this
    // machine that is what makes the driver answer E_INVALIDARG (0x80070057) to every
    // initialisation, including the engine's own mix format, while the .NET build -
    // which never calls SetClientProperties - opens the very same endpoint. The plain
    // initialise below is the whole of it.
    FormatDescription negotiated;
    WAVEFORMATEX plain {};
    WAVEFORMATEXTENSIBLE extensible {};
    MixFormatPtr mix_format;
    const WAVEFORMATEX* format_ptr = nullptr;
    if (candidate.use_mix_format) {
        // IAudioClient::Initialize does NOT accept a null format: a shared-mode
        // stream must be initialised WITH the engine's mix format, so
        // GetMixFormat has to come first. Passing nullptr (which is only valid
        // for IAudioClient3's GetSharedModeEnginePeriod-style calls) made every
        // mix-format candidate fail with E_POINTER 0x80004003, and the whole
        // capture start() then reported "no usable WASAPI capture configuration"
        // on a machine whose microphone enumerates fine.
        WAVEFORMATEX* raw_mix = nullptr;
        const HRESULT mix_status = client->GetMixFormat(&raw_mix);
        if (FAILED(mix_status) || raw_mix == nullptr) {
            ::CoTaskMemFree(raw_mix);
            failure = mix_status;
            diagnostics += diagnostic_line(label + " get-mix-format", mix_status) + "; ";
            return false;
        }
        mix_format.reset(raw_mix);
        format_ptr = mix_format.get();
    } else if (candidate.extensible) {
        extensible = make_extensible_pcm16(options.sample_rate, options.channel_count);
        format_ptr = reinterpret_cast<const WAVEFORMATEX*>(&extensible);
    } else {
        plain = make_pcm16(options.sample_rate, options.channel_count);
        format_ptr = &plain;
    }

    // No NOPERSIST: the reference implementation that works on this hardware does
    // not set it, and every extra flag on this driver is one more E_INVALIDARG.
    DWORD stream_flags = 0;
    if (candidate.event_driven) {
        stream_flags |= AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    }
    if (candidate.auto_convert) {
        stream_flags |= kStreamFlagsAutoConvertPcm | kStreamFlagsSrcDefaultQuality;
    }
    const REFERENCE_TIME buffer_duration =
        static_cast<REFERENCE_TIME>(candidate.buffer_milliseconds) * 10000LL;

    // An exclusive-mode event-driven stream requires hnsPeriodicity to equal
    // hnsBufferDuration; every other combination must pass zero.
    const REFERENCE_TIME periodicity
        = (candidate.exclusive && candidate.event_driven) ? buffer_duration : 0;
    hr = client->Initialize(
        candidate.exclusive ? AUDCLNT_SHAREMODE_EXCLUSIVE : AUDCLNT_SHAREMODE_SHARED,
        stream_flags,
        buffer_duration,
        periodicity,
        format_ptr,
        nullptr);
    if (FAILED(hr)) {
        failure = hr;
        diagnostics += diagnostic_line(label + " init", hr) + "; ";
        return false;
    }

    // The format the engine actually accepted is what the converter is built
    // from: for the mix-format candidate it is the engine's own answer, for the
    // others the format we asked for (Windows rejects an unsupported one in
    // Initialize instead of silently converting).
    negotiated = describe_format(*format_ptr);
    if (!negotiated.valid) {
        failure = AUDCLNT_E_UNSUPPORTED_FORMAT;
        diagnostics += label + (candidate.use_mix_format ? " mix-format-not-supported; "
                                                         : " requested-format-invalid; ");
        return false;
    }

    ComPtr<IAudioCaptureClient> capture;
    hr = client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(capture.put()));
    if (FAILED(hr)) {
        failure = hr;
        diagnostics += diagnostic_line(label + " get-service", hr) + "; ";
        return false;
    }

    void* render_event = nullptr;
    if (candidate.event_driven) {
        render_event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (render_event == nullptr) {
            failure = HRESULT_FROM_WIN32(::GetLastError());
            diagnostics += diagnostic_line(label + " create-event", failure) + "; ";
            return false;
        }
        hr = client->SetEventHandle(static_cast<HANDLE>(render_event));
        if (FAILED(hr)) {
            ::CloseHandle(static_cast<HANDLE>(render_event));
            failure = hr;
            diagnostics += diagnostic_line(label + " set-event-handle", hr) + "; ";
            return false;
        }
    }

    hr = client->Start();
    if (FAILED(hr)) {
        if (render_event != nullptr) {
            ::CloseHandle(static_cast<HANDLE>(render_event));
        }
        failure = hr;
        diagnostics += diagnostic_line(label + " start", hr) + "; ";
        return false;
    }

    // Everything succeeded: publish the session. From here on the capture thread
    // owns the COM objects and the sink.
    client_ = std::move(client);
    capture_client_ = std::move(capture);
    render_event_ = render_event;
    device_format_ = negotiated.format;
    bytes_per_frame_ = negotiated.format.bytes_per_frame();
    event_driven_ = candidate.event_driven;
    end_reason_.store(WasapiEndReason::completed, std::memory_order_release);
    return true;
}

void WasapiCaptureImpl::wait_for_stop() noexcept
{
    if (event_driven_) {
        HANDLE handles[2] = { static_cast<HANDLE>(stop_event_), static_cast<HANDLE>(render_event_) };
        ::WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        return;
    }
    ::WaitForSingleObject(static_cast<HANDLE>(stop_event_), static_cast<DWORD>(idle_wait_.count()));
}

bool WasapiCaptureImpl::reuse_session(std::string& diagnostics)
{
    ComScope com;
    if (!com.available()) {
        return false;
    }
    diagnostics += "com=" + apartment_name(com.result()) + "; reused; ";

    if (event_driven_) {
        // close_events() ran in stop(), so the client has no handle to signal on
        // and has to be told again before Start().
        render_event_ = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (render_event_ == nullptr) {
            return false;
        }
        const HRESULT set_event = client_->SetEventHandle(static_cast<HANDLE>(render_event_));
        if (FAILED(set_event)) {
            diagnostics += diagnostic_line("reused set-event-handle", set_event) + "; ";
            ::CloseHandle(static_cast<HANDLE>(render_event_));
            render_event_ = nullptr;
            return false;
        }
    }

    const HRESULT started = client_->Start();
    if (FAILED(started)) {
        diagnostics += diagnostic_line("reused start", started) + "; ";
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        report_.share_mode = negotiated_exclusive_ ? WasapiShareMode::exclusive : WasapiShareMode::shared;
        report_.event_driven = event_driven_;
        report_.buffer_milliseconds = negotiated_buffer_ms_;
        report_.accepted_candidate = negotiated_candidate_ + 1;
        report_.device_id = last_device_id_;
    }
    return true;
}

void WasapiCaptureImpl::capture_loop() noexcept
{
    {
        // The capture thread uses COM, so it initializes COM itself. A thread
        // that is refused ends the session as a device failure; it never leaves
        // COM uninitialized.
        ComScope com;
        if (!com.available()) {
            end_reason_.store(WasapiEndReason::device_lost, std::memory_order_release);
            return;
        }

        try {
            while (true) {
                if (::WaitForSingleObject(static_cast<HANDLE>(stop_event_), 0) == WAIT_OBJECT_0) {
                    break;
                }
                if (cancellation_.is_cancellation_requested()) {
                    end_reason_.store(WasapiEndReason::cancelled, std::memory_order_release);
                    break;
                }

                UINT32 available = 0;
                const HRESULT next = capture_client_->GetNextPacketSize(&available);
                if (FAILED(next)) {
                    // AUDCLNT_E_BUFFER_ERROR / AUDCLNT_E_OUT_OF_ORDER / device
                    // invalidated: the stream is dead, end the session.
                    end_reason_.store(WasapiEndReason::device_lost, std::memory_order_release);
                    break;
                }
                if (available == 0) {
                    wait_for_stop();
                    continue;
                }

                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                const HRESULT buffer_status = capture_client_->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
                if (FAILED(buffer_status)) {
                    end_reason_.store(WasapiEndReason::device_lost, std::memory_order_release);
                    break;
                }

                const auto bytes = static_cast<std::uint32_t>(frames * bytes_per_frame_);
                const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                bool sink_ok = true;
                if (sink_ != nullptr && frames > 0) {
                    try {
                        if (silent) {
                            // The engine produced silence instead of device data.
                            // Hand the sink a real zero buffer (the engine packet
                            // is write-only in that case) and, in event-driven
                            // mode, write the silence the contract requires before
                            // releasing the buffer.
                            silence_.assign(bytes, std::byte { 0 });
                            if (event_driven_ && data != nullptr) {
                                std::memset(data, 0, bytes);
                            }
                            const WasapiPacket packet {
                                silence_.data(), bytes, frames, device_format_, true
                            };
                            sink_(packet);
                        } else {
                            const WasapiPacket packet {
                                reinterpret_cast<const std::byte*>(data), bytes, frames, device_format_, false
                            };
                            sink_(packet);
                        }
                    } catch (...) {
                        sink_ok = false;
                    }
                }

                // ReleaseBuffer must run even when the sink threw, otherwise the
                // buffer never goes back to the client and Stop() would fail.
                const HRESULT released = capture_client_->ReleaseBuffer(frames);
                if (!sink_ok) {
                    end_reason_.store(WasapiEndReason::sink_failed, std::memory_order_release);
                    break;
                }
                if (FAILED(released)) {
                    end_reason_.store(WasapiEndReason::device_lost, std::memory_order_release);
                    break;
                }
            }
        } catch (...) {
            // Nothing may escape the capture thread: the owner waits on
            // thread_finished_ and would otherwise hang in stop().
            end_reason_.store(WasapiEndReason::device_lost, std::memory_order_release);
        }
    }

    {
        std::lock_guard<std::mutex> lock(finished_mutex_);
        thread_finished_.store(true, std::memory_order_release);
    }
    finished_cv_.notify_all();
}

Status WasapiCaptureImpl::stop()
{
    if (!capturing_ && !thread_.joinable()) {
        return Status::success();
    }
    capturing_ = false;

    // 1. device: signal the loop, then wait for the capture thread to leave
    //    IAudioClient alone. Bounded, so a wedged COM call cannot hang the UI.
    if (stop_event_ != nullptr) {
        ::SetEvent(static_cast<HANDLE>(stop_event_));
    }
    bool joined = false;
    {
        std::unique_lock<std::mutex> lock(finished_mutex_);
        joined = finished_cv_.wait_for(
            lock, stop_timeout_, [this] { return thread_finished_.load(std::memory_order_acquire); });
    }
    if (!joined) {
        // The loop polls the stop event every idle_wait, so this is a defect
        // guard, not a normal path. The object must not be destroyed while the
        // thread lives; the caller retries stop() and destroy() joins anyway.
        return Status::failure(domain::Error(
            ErrorCode::io_failure, "the WASAPI capture thread did not stop", "join-timeout"));
    }
    if (thread_.joinable()) {
        thread_.join();
    }

    // 2. device: the client stops only after the thread is gone, so Stop() can
    //    never race GetBuffer/ReleaseBuffer.
    if (client_ != nullptr) {
        client_->Stop();
    }

    // 3. unsubscribe: only now may the caller's buffer be snapshotted.
    sink_ = nullptr;
    close_events();
    return Status::success();
}

void WasapiCaptureImpl::close_events() noexcept
{
    if (render_event_ != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(render_event_));
        render_event_ = nullptr;
    }
}

void WasapiCaptureImpl::release_session() noexcept
{
    capture_client_.reset();
    client_.reset();
    device_.reset();
    silence_.clear();
    silence_.shrink_to_fit();
    bytes_per_frame_ = 0;
    event_driven_ = false;
}

void WasapiCaptureImpl::destroy() noexcept
{
    if (capturing_) {
        (void)stop();
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    release_session();
    close_events();
    if (stop_event_ != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(stop_event_));
        stop_event_ = nullptr;
    }
    sink_ = nullptr;
    capturing_ = false;
}

Status WasapiCaptureImpl::start(
    const WasapiCaptureOptions& options, WasapiPacketSink sink, const CancellationToken& cancellation)
{
    if (capturing_) {
        return Status::failure(ErrorCode::invalid_state, "a capture session is already running");
    }
    if (sink == nullptr) {
        return Status::failure(ErrorCode::invalid_argument, "a packet sink is required");
    }
    if (cancellation.is_cancellation_requested()) {
        return Status::failure(ErrorCode::cancelled, "cancelled before the device was opened");
    }

    std::string diagnostics;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        report_ = WasapiCaptureReport {};
    }

    // Reuse the negotiated client when the same device and strategy are asked for
    // again: that is the difference between a dictation that starts immediately
    // and one that loses its first second to a fresh negotiation.
    bool reused = false;
    const bool same_target = client_ != nullptr && capture_client_ != nullptr
        && negotiated_strategy_.has_value() && *negotiated_strategy_ == options.strategy
        && requested_device_id_ == options.device_id;
    if (same_target) {
        reused = reuse_session(diagnostics);
    }
    if (!reused) {
        // A different device, a different strategy, or a failed reuse: negotiate
        // from a clean set of COM objects and events.
        close_events();
        release_session();
    }
    if (stop_event_ != nullptr) {
        // Manual-reset and set by the previous stop(): the loop would exit at once
        // if it were reused.
        ::CloseHandle(static_cast<HANDLE>(stop_event_));
        stop_event_ = nullptr;
    }

    // One COM apartment for the whole negotiation. resolve_device() used to
    // create and then destroy its own scope, so by the time try_candidate()
    // reached IAudioClient::Initialize the thread had no apartment left, and
    // every exclusive attempt came back as CO_E_NOTINITIALIZED 0x800401F0 while
    // the shared ones reported parameter errors.
    if (!reused) {
    ComScope com;
    if (!com.available()) {
        return failure_status(com.failure(), "COM is not available on this thread",
            diagnostic_line("coinitialize", com.failure()));
    }
    diagnostics += "com=" + apartment_name(com.result()) + "; ";

    const Status device_status = resolve_device(options, diagnostics);
    if (device_status.is_error()) {
        std::lock_guard<std::mutex> lock(mutex_);
        report_.diagnostics = diagnostics;
        return device_status.with_context(diagnostics);
    }

    LPWSTR endpoint_id = nullptr;
    if (SUCCEEDED(device_->GetId(&endpoint_id)) && endpoint_id != nullptr) {
        const std::string id = narrow_utf8(endpoint_id);
        last_device_id_ = id;
        std::lock_guard<std::mutex> lock(mutex_);
        report_.device_id = id;
    }
    ::CoTaskMemFree(endpoint_id);

    const std::vector<Candidate> candidates = build_candidates(options.strategy);

    HRESULT last_failure = AUDCLNT_E_ENDPOINT_CREATE_FAILED;
    bool accepted = false;
    // The candidate sweep runs against the selected endpoint. It is a lambda because
    // the .NET build - the implementation that works on this hardware - resolves the
    // endpoint BY ROLE (RawWasapiCapture.ResolveDevice(role) with eConsole first) and
    // never insists on the stored id: on this machine the stored id is a stale
    // "Intel Smart Sound" endpoint that reports itself active but refuses every
    // initialisation, while the current default endpoint opens fine. So when the
    // stored device yields nothing, the same sweep is run once more on the default
    // endpoint instead of failing the whole capture.
    const auto try_device = [&](const WasapiCaptureOptions& sweep_options) {
        for (std::size_t index = 0; index < candidates.size() && !accepted; ++index) {
            HRESULT candidate_failure = S_OK;
            if (!try_candidate(candidates[index], sweep_options, diagnostics, candidate_failure)) {
                last_failure = candidate_failure;
                continue;
            }
            accepted = true;
            negotiated_strategy_ = sweep_options.strategy;
            requested_device_id_ = sweep_options.device_id;
            negotiated_candidate_ = static_cast<int>(index);
            negotiated_exclusive_ = candidates[index].exclusive;
            negotiated_buffer_ms_ = candidates[index].buffer_milliseconds;
            std::lock_guard<std::mutex> lock(mutex_);
            report_.share_mode = candidates[index].exclusive ? WasapiShareMode::exclusive : WasapiShareMode::shared;
            report_.event_driven = candidates[index].event_driven;
            report_.buffer_milliseconds = candidates[index].buffer_milliseconds;
            report_.accepted_candidate = static_cast<int>(index) + 1;
        }
    };

    try_device(options);

    if (!accepted && !options.device_id.empty()) {
        ComPtr<IMMDeviceEnumerator> enumerator;
        const HRESULT enum_status = ::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(enumerator.put()));
        if (SUCCEEDED(enum_status)) {
            const ERole roles[] = { eConsole, eCommunications };
            for (ERole role : roles) {
                ComPtr<IMMDevice> fallback_device;
                if (FAILED(enumerator->GetDefaultAudioEndpoint(eCapture, role, fallback_device.put()))) {
                    continue;
                }
                if (fallback_device.get() == device_.get()) {
                    continue;
                }
                diagnostics += std::string(role == eConsole ? "default-console" : "default-communications")
                    + " fallback: the selected microphone did not open; ";
                device_ = std::move(fallback_device);
                WasapiCaptureOptions fallback_options = options;
                fallback_options.device_id.clear();
                try_device(fallback_options);
                if (accepted) {
                    break;
                }
            }
        }
    }

    if (!accepted) {
        release_session();
        close_events();
        std::lock_guard<std::mutex> lock(mutex_);
        report_.diagnostics = diagnostics;
        return failure_status(
            last_failure,
            "no usable WASAPI capture configuration",
            diagnostics.empty() ? std::string("no candidates") : diagnostics);
    }
    } // if (!reused)

    stop_event_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (stop_event_ == nullptr) {
        release_session();
        return Status::failure(ErrorCode::internal, "could not create the capture stop event");
    }

    sink_ = std::move(sink);
    cancellation_ = cancellation;
    idle_wait_ = options.idle_wait.count() > 0 ? options.idle_wait : std::chrono::milliseconds(1);
    stop_timeout_ = options.stop_timeout.count() > 0 ? options.stop_timeout : std::chrono::milliseconds(2000);
    thread_finished_.store(false, std::memory_order_release);
    capturing_ = true;

    try {
        thread_ = std::thread([this] { capture_loop(); });
    } catch (const std::exception&) {
        capturing_ = false;
        sink_ = nullptr;
        release_session();
        ::CloseHandle(static_cast<HANDLE>(stop_event_));
        stop_event_ = nullptr;
        return Status::failure(ErrorCode::resource_exhausted, "could not start the WASAPI capture thread");
    }

    std::lock_guard<std::mutex> lock(mutex_);
    report_.diagnostics = diagnostics;
    return Status::success();
}

} // namespace

Result<std::unique_ptr<WasapiCapture>> WasapiCapture::create()
{
    return Result<std::unique_ptr<WasapiCapture>>(std::unique_ptr<WasapiCapture>(new WasapiCaptureImpl()));
}

} // namespace voicetyper::platform
