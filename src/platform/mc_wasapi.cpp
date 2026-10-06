#include "platform/mc_wasapi.hpp"

#include <string>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace voicetyper::platform {

namespace {

/// The one session the library's callback can reach. mc_start takes no user data, so
/// the DLL's own contract is a single live session; the pointer mirrors that.
McWasapiSession* g_active_session = nullptr;

void mc_trampoline(const void* data, int bytes, int rate, int channels)
{
    if (g_active_session == nullptr) {
        return;
    }
    g_active_session->deliver(data, bytes, rate, channels);
}

} // namespace

McWasapiApi load_mc_wasapi()
{
#if defined(_WIN32)
    // The executable's own directory first: the library is deployed next to the
    // application, and a copy elsewhere on PATH must not shadow it.
    HMODULE module = ::LoadLibraryExW(L"mc_wasapi.dll", nullptr,
        LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
    if (module == nullptr) {
        module = ::LoadLibraryW(L"mc_wasapi.dll");
    }
    if (module == nullptr) {
        return {};
    }
    McWasapiApi api;
    api.start = reinterpret_cast<int (*)(McWasapiApi::Callback, int, int)>(
        reinterpret_cast<void*>(::GetProcAddress(module, "mc_start")));
    api.stop = reinterpret_cast<void (*)()>(
        reinterpret_cast<void*>(::GetProcAddress(module, "mc_stop")));
    if (!api.available()) {
        return {};
    }
    return api;
#else
    return {};
#endif
}

std::string mc_wasapi_failure_detail(int code)
{
    switch (code) {
    case 0:
        return "the native capture started";
    case 1:
        return "mc_wasapi.dll could not open the capture device";
    case 2:
        return "mc_wasapi.dll is already capturing";
    case 3:
        return "the capture thread of mc_wasapi.dll could not be created";
    default:
        break;
    }
    return "mc_wasapi.dll reported failure code " + std::to_string(code);
}

McWasapiSession::McWasapiSession(McWasapiApi api, std::uint32_t rate, std::uint16_t channels)
    : api_(api)
    , rate_(rate)
    , channels_(channels)
{
}

McWasapiSession::~McWasapiSession()
{
    stop();
}

Status McWasapiSession::start(Sink sink)
{
    if (!api_.available()) {
        return Status::failure(ErrorCode::unavailable,
            "the native capture library mc_wasapi.dll is not available");
    }
    if (running_) {
        return Status::failure(ErrorCode::invalid_state, "this capture session is already running");
    }
    if (g_active_session != nullptr) {
        return Status::failure(ErrorCode::invalid_state,
            "another capture session is running: mc_wasapi.dll allows one at a time");
    }
    if (!sink) {
        return Status::failure(ErrorCode::internal, "a capture session needs a sink");
    }
    // The state is published BEFORE the library is asked to start: mc_wasapi.dll
    // begins delivering on its own thread as soon as it is running, and a buffer that
    // arrives in that window would otherwise find no session and be dropped - which
    // is the first thing a capture loses.
    sink_ = std::move(sink);
    running_ = true;
    g_active_session = this;
    last_code_ = api_.start(&mc_trampoline, static_cast<int>(rate_), static_cast<int>(channels_));
    if (last_code_ != 0) {
        running_ = false;
        g_active_session = nullptr;
        sink_ = nullptr;
        return Status::failure(ErrorCode::resource_exhausted, mc_wasapi_failure_detail(last_code_));
    }
    return Status::success();
}

void McWasapiSession::stop()
{
    if (!running_) {
        return;
    }
    // The library first, then the bookkeeping: the callback must not reach a session
    // that already considers itself stopped.
    running_ = false;
    if (g_active_session == this) {
        g_active_session = nullptr;
    }
    if (api_.stop != nullptr) {
        api_.stop();
    }
    sink_ = nullptr;
}

bool McWasapiSession::running() const noexcept
{
    return running_;
}

int McWasapiSession::last_code() const noexcept
{
    return last_code_;
}

void McWasapiSession::deliver(const void* data, int bytes, int rate, int channels)
{
    if (!running_ || data == nullptr || bytes <= 0 || !sink_) {
        return;
    }
    sink_(data, bytes, rate, channels);
}

} // namespace voicetyper::platform
