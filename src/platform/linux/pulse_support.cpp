#include "platform/linux/pulse_support.hpp"

#include <utility>

namespace voicetyper::platform::linuxos {
namespace {

/// The server info of one default_source_name() call.
struct ServerInfoRequest {
    std::string default_source;
    bool done = false;
};

void server_info_callback(pa_context*, const pa_server_info* info, void* userdata)
{
    auto* request = static_cast<ServerInfoRequest*>(userdata);
    if (info != nullptr && info->default_source_name != nullptr) {
        request->default_source = info->default_source_name;
    }
    request->done = true;
}

} // namespace

PulseConnection::PulseConnection()
{
    loop_ = pa_mainloop_new();
    if (loop_ == nullptr) {
        return;
    }
    context_ = pa_context_new(pa_mainloop_get_api(loop_), kPulseClientName);
    if (context_ == nullptr) {
        pa_mainloop_free(loop_);
        loop_ = nullptr;
    }
}

PulseConnection::~PulseConnection()
{
    if (context_ != nullptr) {
        pa_context_disconnect(context_);
        pa_context_unref(context_);
    }
    if (loop_ != nullptr) {
        pa_mainloop_free(loop_);
    }
}

std::string PulseConnection::last_error(pa_context* context)
{
    if (context == nullptr) {
        return "no sound server context";
    }
    const char* text = pa_strerror(pa_context_errno(context));
    return text != nullptr ? std::string(text) : std::string("unknown sound server error");
}

bool PulseConnection::connect(std::chrono::milliseconds timeout, std::string& error)
{
    if (context_ == nullptr || loop_ == nullptr) {
        error = "the sound server main loop could not be created";
        return false;
    }
    if (pa_context_connect(context_, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0) {
        error = last_error(context_);
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const pa_context_state_t state = pa_context_get_state(context_);
        if (state == PA_CONTEXT_READY) {
            ready_ = true;
            return true;
        }
        if (!PA_CONTEXT_IS_GOOD(state)) {
            error = last_error(context_);
            return false;
        }
        iterate(std::chrono::milliseconds(10));
    }
    error = "the sound server did not become ready within "
        + std::to_string(timeout.count()) + " ms";
    return false;
}

void PulseConnection::iterate(std::chrono::milliseconds timeout)
{
    if (loop_ == nullptr) {
        return;
    }
    int retval = 0;
    // pa_mainloop_iterate blocks until an event arrives, so it is given a
    // deadline through the poll timeout instead of being called bare.
    pa_mainloop_prepare(loop_, static_cast<int>(timeout.count()));
    pa_mainloop_poll(loop_);
    pa_mainloop_dispatch(loop_);
    static_cast<void>(retval);
}

bool PulseConnection::wait(pa_operation* operation, std::chrono::milliseconds timeout, std::string& error)
{
    if (operation == nullptr) {
        error = last_error(context_);
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    bool finished = false;
    while (std::chrono::steady_clock::now() < deadline) {
        const pa_operation_state_t state = pa_operation_get_state(operation);
        if (state == PA_OPERATION_DONE) {
            finished = true;
            break;
        }
        if (state == PA_OPERATION_CANCELLED) {
            error = "the sound server cancelled the request";
            break;
        }
        iterate(std::chrono::milliseconds(10));
    }
    pa_operation_unref(operation);
    if (!finished) {
        if (error.empty()) {
            error = "the sound server did not answer within "
                + std::to_string(timeout.count()) + " ms";
        }
        return false;
    }
    return true;
}

std::string PulseConnection::default_source_name(std::chrono::milliseconds timeout, std::string& error)
{
    if (!connected()) {
        error = "not connected to the sound server";
        return {};
    }
    ServerInfoRequest request;
    pa_operation* operation = pa_context_get_server_info(context_, &server_info_callback, &request);
    if (!wait(operation, timeout, error)) {
        return {};
    }
    return request.default_source;
}

} // namespace voicetyper::platform::linuxos
