// IMMDeviceEnumerator microphone enumeration. Companion to windows_microphone.hpp,
// which documents the contract; this file holds every IMM*/PROP* type.
//
// Two behaviours are load-bearing and are tested:
//   * an empty device list means "no active capture endpoint" and is a success,
//     whereas a refused enumeration is reported as unavailable/permission_denied
//     through last_error() - the .NET service swallowed both into one empty
//     list, which is exactly the diagnostic gap this port closes;
//   * the default flag is resolved by comparing endpoint ids rather than by
//     trusting enumeration order, so the settings dropdown can mark the real
//     default even when it is not the first entry.

#include "platform/windows/windows_microphone.hpp"

#if !defined(_WIN32)
#error "windows_microphone.cpp is Windows platform code; the target must be WIN32-only"
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

#include <string>
#include <utility>
#include <vector>

namespace voicetyper::platform {
namespace {

/// PKEY_Device_FriendlyName = {A45C254E-DF1C-4EFD-8020-67D146A850E0}, 14.
/// The property every endpoint answers with; PKEY_DeviceInterface_FriendlyName
/// (the same fmtid, pid 2) is the documented fallback.
constexpr PROPERTYKEY kPkeyDeviceFriendlyName = {
    { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } },
    14
};
constexpr PROPERTYKEY kPkeyDeviceInterfaceFriendlyName = {
    { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } },
    2
};

/// Owning COM pointer; see wasapi_capture.cpp for why every interface handle in
/// this backend is released on every path.
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

class ComScope {
public:
    ComScope() noexcept
    {
        const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(hr)) {
            owned_ = true;
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

private:
    bool owned_ = false;
    bool available_ = false;
    HRESULT failure_ = S_OK;
};

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
        return ErrorCode::device_disconnected;
    case static_cast<DWORD>(AUDCLNT_E_SERVICE_NOT_RUNNING):
    case static_cast<DWORD>(kRpcServerUnavailable):
    case static_cast<DWORD>(CO_E_NOTINITIALIZED):
    case static_cast<DWORD>(CO_E_CLASSSTRING):
    case static_cast<DWORD>(kClassNotAvailable):
        return ErrorCode::unavailable;
    case static_cast<DWORD>(E_NOINTERFACE):
    case static_cast<DWORD>(E_NOTIMPL):
        return ErrorCode::unsupported;
    default:
        return ErrorCode::io_failure;
    }
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
    std::string out(static_cast<std::size_t>(needed), '\0');
    const int written = ::WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), needed, nullptr, nullptr);
    if (written <= 0) {
        return {};
    }
    out.resize(static_cast<std::size_t>(written - 1));
    return out;
}

/// IMMDeviceEnumerator::GetDevice takes the endpoint id as UTF-16, while the
/// persisted settings value is UTF-8. Empty on a conversion failure, which the
/// caller reports as "not present" rather than passing a half-converted id.
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

/// RAII for the LPWSTR that IMMDevice::GetId allocates with CoTaskMemAlloc.
std::string take_device_id(IMMDevice* device)
{
    LPWSTR id = nullptr;
    if (FAILED(device->GetId(&id)) || id == nullptr) {
        return {};
    }
    const std::string text = narrow_utf8(id);
    ::CoTaskMemFree(id);
    return text;
}

Result<ComPtr<IMMDeviceEnumerator>> create_enumerator()
{
    ComPtr<IMMDeviceEnumerator> enumerator;
    // `__uuidof(MMDeviceEnumerator)` is the coclass CLSID; `__uuidof(IMMDeviceEnumerator)`
    // is the interface IID, which is NOT a registered COM class. Passing the IID
    // as the class id makes CoCreateInstance return REGDB_E_CLASSNOTREG
    // (0x80040154), mapped below to `unavailable`, and the app then reports
    // "devices=0" on a machine whose microphone list PowerShell reads fine -
    // measured on the target machine 2026-10-01, which is why the log now also
    // carries this diagnostic.
    const HRESULT hr = ::CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        nullptr,
        CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator),
        reinterpret_cast<void**>(enumerator.put()));
    if (FAILED(hr)) {
        return Result<ComPtr<IMMDeviceEnumerator>>::failure(
            error_code_from_hresult(hr), "MMDeviceEnumerator is not available (" + hex32(hr) + ")");
    }
    return Result<ComPtr<IMMDeviceEnumerator>>(std::move(enumerator));
}

Result<std::string> default_endpoint_id(IMMDeviceEnumerator* enumerator)
{
    const ERole roles[] = { eConsole, eCommunications };
    HRESULT last = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    for (ERole role : roles) {
        ComPtr<IMMDevice> device;
        const HRESULT hr = enumerator->GetDefaultAudioEndpoint(eCapture, role, device.put());
        if (FAILED(hr)) {
            last = hr;
            continue;
        }
        const std::string id = take_device_id(device.get());
        if (!id.empty()) {
            return Result<std::string>(id);
        }
    }
    return Result<std::string>(domain::Error(
        error_code_from_hresult(last), "no default capture endpoint", hex32(last)));
}

struct NameResult {
    std::string name;
    /// True when the OS refused to hand over a name (privacy settings).
    bool denied = false;
};

/// Reads the friendly name. A missing property falls back to the interface-id
/// tail (readable enough for a settings row) and an access denial is reported
/// separately, because "the name is hidden" and "there is no name" are
/// different user-visible states.
NameResult read_friendly_name(IMMDevice* device)
{
    NameResult result;
    ComPtr<IPropertyStore> store;
    if (FAILED(device->OpenPropertyStore(STGM_READ, store.put()))) {
        return result;
    }

    const PROPERTYKEY keys[] = { kPkeyDeviceFriendlyName, kPkeyDeviceInterfaceFriendlyName };
    for (const PROPERTYKEY& key : keys) {
        PROPVARIANT value {};
        ::PropVariantInit(&value);
        const HRESULT hr = store->GetValue(key, &value);
        if (FAILED(hr)) {
            if (hr == E_ACCESSDENIED) {
                result.denied = true;
            }
            ::PropVariantClear(&value);
            continue;
        }
        // A device friendly name is always VT_LPWSTR. A BSTR is not accepted
        // on purpose: it would need OLEAUTOMAT string handling for a shape the
        // endpoint never returns, and a wrong guess here would put a leaked or
        // misread string into the settings dropdown.
        if (value.vt == VT_LPWSTR && value.pwszVal != nullptr) {
            result.name = narrow_utf8(value.pwszVal);
        }
        ::PropVariantClear(&value);
        if (!result.name.empty()) {
            return result;
        }
    }
    return result;
}

/// Best-effort mix format of an endpoint. The contract allows an invalid
/// (rate 0) format when the backend could not determine one, so a refused probe
/// is not an error; it only leaves native_format unset.
domain::AudioFormat probe_mix_format(IMMDevice* device)
{
    ComPtr<IAudioClient> client;
    if (FAILED(device->Activate(
            __uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.put())))) {
        return {};
    }
    WAVEFORMATEX* mix = nullptr;
    if (FAILED(client->GetMixFormat(&mix)) || mix == nullptr) {
        ::CoTaskMemFree(mix);
        return {};
    }
    const domain::AudioFormat format(
        static_cast<std::uint32_t>(mix->nSamplesPerSec),
        static_cast<std::uint16_t>(mix->nChannels),
        mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ? domain::SampleFormat::ieee_float32
                                                  : domain::SampleFormat::pcm_s16);
    const bool usable = format.validate().is_ok();
    ::CoTaskMemFree(mix);
    return usable ? format : domain::AudioFormat();
}

} // namespace

bool is_default_microphone_id(std::string_view device_id)
{
    return device_id.empty() || device_id == kDefaultMicrophoneDeviceId;
}

std::vector<MicrophoneDevice> WindowsMicrophone::list_devices()
{
    last_error_ = Error();
    diagnostics_.clear();

    ComScope com;
    if (!com.available()) {
        last_error_ = Error(
            error_code_from_hresult(com.failure()),
            "COM is not available on this thread",
            hex32(com.failure()));
        diagnostics_ = "microphone enumeration: " + last_error_.to_string();
        return {};
    }

    auto enumerator_result = create_enumerator();
    if (enumerator_result.is_error()) {
        last_error_ = enumerator_result.error();
        diagnostics_ = "microphone enumeration: " + last_error_.to_string();
        return {};
    }
    ComPtr<IMMDeviceEnumerator> enumerator = std::move(enumerator_result).value();

    ComPtr<IMMDeviceCollection> collection;
    HRESULT hr = enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, collection.put());
    if (FAILED(hr)) {
        last_error_ = Error(
            error_code_from_hresult(hr), "enumerating capture endpoints failed", hex32(hr));
        diagnostics_ = "microphone enumeration: " + last_error_.to_string();
        return {};
    }

    const std::string default_id = [&] {
        auto id = default_endpoint_id(enumerator.get());
        return id.is_ok() ? id.value() : std::string();
    }();

    UINT32 count = 0;
    hr = collection->GetCount(&count);
    if (FAILED(hr)) {
        last_error_ = Error(error_code_from_hresult(hr), "counting capture endpoints failed", hex32(hr));
        diagnostics_ = "microphone enumeration: " + last_error_.to_string();
        return {};
    }

    std::vector<MicrophoneDevice> devices;
    devices.reserve(count);
    bool any_name_denied = false;
    for (UINT32 index = 0; index < count; ++index) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(index, device.put())) || device == nullptr) {
            continue;
        }
        const std::string id = take_device_id(device.get());
        if (id.empty()) {
            continue;
        }

        const NameResult named = read_friendly_name(device.get());
        any_name_denied = any_name_denied || named.denied;

        MicrophoneDevice entry;
        entry.id = id;
        entry.name = named.name.empty() ? id : named.name;
        entry.is_default = (!default_id.empty() && id == default_id);
        entry.native_format = probe_mix_format(device.get());
        devices.push_back(std::move(entry));
    }

    if (devices.empty()) {
        // "The machine has no microphone" and "the microphone exists but is
        // disabled/unplugged" are different user-visible states, so the second
        // case is looked for explicitly and reported instead of a bare empty
        // list.
        ComPtr<IMMDeviceCollection> all_states;
        if (SUCCEEDED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATEMASK_ALL, all_states.put()))) {
            UINT32 all_count = 0;
            if (SUCCEEDED(all_states->GetCount(&all_count)) && all_count > 0) {
                last_error_ = Error(
                    ErrorCode::not_found,
                    "no active capture endpoint",
                    std::to_string(all_count) + " present but not active");
                diagnostics_ = "microphone enumeration: " + last_error_.to_string();
            }
        }
    }

    if (any_name_denied) {
        // The list is still usable (ids as names), but the caller is told that
        // the OS is hiding device names, which is a privacy-setting problem the
        // user can act on.
        last_error_ = Error(
            ErrorCode::permission_denied, "the operating system hid the microphone names", "E_ACCESSDENIED");
        diagnostics_ = "microphone enumeration: " + last_error_.to_string();
    }
    return devices;
}

std::string WindowsMicrophone::default_device_id()
{
    last_error_ = Error();
    diagnostics_.clear();

    ComScope com;
    if (!com.available()) {
        last_error_ = Error(
            error_code_from_hresult(com.failure()), "COM is not available on this thread", hex32(com.failure()));
        diagnostics_ = "default microphone: " + last_error_.to_string();
        return {};
    }

    auto enumerator_result = create_enumerator();
    if (enumerator_result.is_error()) {
        last_error_ = enumerator_result.error();
        diagnostics_ = "default microphone: " + last_error_.to_string();
        return {};
    }
    ComPtr<IMMDeviceEnumerator> enumerator = std::move(enumerator_result).value();

    auto id = default_endpoint_id(enumerator.get());
    if (id.is_error()) {
        last_error_ = id.error();
        diagnostics_ = "default microphone: " + last_error_.to_string();
        return {};
    }
    return id.value();
}

bool WindowsMicrophone::is_device_available(const std::string& device_id)
{
    last_error_ = Error();
    diagnostics_.clear();

    ComScope com;
    if (!com.available()) {
        last_error_ = Error(
            error_code_from_hresult(com.failure()), "COM is not available on this thread", hex32(com.failure()));
        diagnostics_ = "microphone availability: " + last_error_.to_string();
        return false;
    }

    auto enumerator_result = create_enumerator();
    if (enumerator_result.is_error()) {
        last_error_ = enumerator_result.error();
        diagnostics_ = "microphone availability: " + last_error_.to_string();
        return false;
    }
    ComPtr<IMMDeviceEnumerator> enumerator = std::move(enumerator_result).value();

    if (is_default_microphone_id(device_id)) {
        const auto id = default_endpoint_id(enumerator.get());
        if (id.is_error()) {
            last_error_ = id.error();
            diagnostics_ = "microphone availability: " + last_error_.to_string();
        }
        return id.is_ok();
    }

    ComPtr<IMMDevice> device;
    const std::wstring wide_id = widen_utf8(device_id);
    if (wide_id.empty()) {
        last_error_ = Error(ErrorCode::invalid_argument, "the stored microphone id is not valid UTF-8", device_id);
        diagnostics_ = "microphone availability: " + last_error_.to_string();
        return false;
    }
    const HRESULT hr = enumerator->GetDevice(wide_id.c_str(), device.put());
    if (FAILED(hr)) {
        last_error_ = Error(error_code_from_hresult(hr), "the selected microphone is not present", hex32(hr));
        diagnostics_ = "microphone availability: " + last_error_.to_string();
        return false;
    }

    DWORD state = 0;
    if (FAILED(device->GetState(&state))) {
        last_error_ = Error(error_code_from_hresult(hr), "the microphone state is unknown", hex32(hr));
        diagnostics_ = "microphone availability: " + last_error_.to_string();
        return false;
    }
    if ((state & DEVICE_STATE_ACTIVE) == 0) {
        last_error_ = Error(ErrorCode::not_found, "the selected microphone is not active", "DEVICE_STATE_ACTIVE unset");
        diagnostics_ = "microphone availability: " + last_error_.to_string();
        return false;
    }
    return true;
}

} // namespace voicetyper::platform
