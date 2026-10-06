// Contract for the mc_wasapi.dll capture path.
//
// The library itself cannot be part of a unit test (it needs Windows and the real
// device), so the api is injected: these checks pin the session's behaviour - the
// bookkeeping around mc_start/mc_stop, the refusal to hijack a live session, and the
// forwarding of the library's buffers. Written before the implementation, per
// docs/tdd.md (ADR-012).

#include "platform/mc_wasapi.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;
int checks = 0;

void check(bool condition, const std::string& what)
{
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("  FAIL %s\n", what.c_str());
    } else {
        std::printf("  ok   %s\n", what.c_str());
    }
}

template <typename T>
void check_equal(const T& actual, const T& expected, const std::string& what)
{
    check(actual == expected, what + " (expected " + std::to_string(expected) + ", got "
            + std::to_string(actual) + ")");
}

/// A stand-in for mc_wasapi.dll: records the calls the session makes.
struct FakeLibrary {
    int start_result = 0;
    int start_calls = 0;
    int stop_calls = 0;
    int rate = 0;
    int channels = 0;
    voicetyper::platform::McWasapiApi::Callback callback = nullptr;

    voicetyper::platform::McWasapiApi api()
    {
        return voicetyper::platform::McWasapiApi{&FakeLibrary::start, &FakeLibrary::stop};
    }

    static int start(voicetyper::platform::McWasapiApi::Callback cb, int rate_value, int channel_value)
    {
        FakeLibrary& self = instance();
        ++self.start_calls;
        self.callback = cb;
        self.rate = rate_value;
        self.channels = channel_value;
        return self.start_result;
    }

    static void stop()
    {
        ++instance().stop_calls;
    }

    /// The single fake the function pointers above reach; the real library has the
    /// same single-session contract, so the test mirrors it.
    static FakeLibrary& instance()
    {
        static FakeLibrary fake;
        return fake;
    }
};

void a_missing_library_fails_without_calling_anything()
{
    voicetyper::platform::McWasapiSession session({}, 48000, 2);
    const auto status = session.start([](const void*, int, int, int) {});
    check(!status.is_ok(), "a session without the library cannot start");
    check_equal(static_cast<int>(status.error().code()),
        static_cast<int>(voicetyper::domain::ErrorCode::unavailable), std::string("the error code"));
    check_equal(session.running(), false, std::string("running after a failed start"));
}

void a_successful_start_and_stop_bookkeeping()
{
    FakeLibrary& fake = FakeLibrary::instance();
    fake = FakeLibrary{};
    voicetyper::platform::McWasapiSession session(fake.api(), 48000, 2);
    const auto status = session.start([](const void*, int, int, int) {});
    check(status.is_ok(), "the session starts when the library returns zero");
    check_equal(fake.start_calls, 1, std::string("mc_start calls"));
    check_equal(fake.rate, 48000, std::string("the requested rate"));
    check_equal(fake.channels, 2, std::string("the requested channels"));
    check_equal(session.running(), true, std::string("running after a successful start"));
    check(fake.callback != nullptr, "the library received a callback");

    session.stop();
    check_equal(fake.stop_calls, 1, std::string("mc_stop calls after one stop"));
    check_equal(session.running(), false, std::string("running after stop"));
    session.stop();
    check_equal(fake.stop_calls, 1, std::string("mc_stop is not called twice"));
}

void a_refused_start_reports_the_library_code()
{
    FakeLibrary& fake = FakeLibrary::instance();
    fake = FakeLibrary{};
    fake.start_result = 5;
    voicetyper::platform::McWasapiSession session(fake.api(), 48000, 2);
    const auto status = session.start([](const void*, int, int, int) {});
    check(!status.is_ok(), "a non-zero mc_start is a failure");
    check_equal(session.last_code(), 5, std::string("the library code"));
    check_equal(session.running(), false, std::string("running after a refused start"));
    session.stop();
    check_equal(fake.stop_calls, 0, std::string("mc_stop is not called for a failed start"));
    check(!voicetyper::platform::mc_wasapi_failure_detail(5).empty(),
        "a refused start has a readable reason");
}

void buffers_reach_the_sink_and_stray_ones_are_ignored()
{
    FakeLibrary& fake = FakeLibrary::instance();
    fake = FakeLibrary{};
    int buffers = 0;
    int last_bytes = 0;
    int last_rate = 0;
    int last_channels = 0;
    voicetyper::platform::McWasapiSession session(fake.api(), 48000, 2);
    static_cast<void>(session.start([&](const void*, int bytes, int rate, int channels) {
        ++buffers;
        last_bytes = bytes;
        last_rate = rate;
        last_channels = channels;
    }));

    const std::byte payload[8] = {};
    session.deliver(payload, 8, 48000, 2);
    check_equal(buffers, 1, std::string("delivered buffers"));
    check_equal(last_bytes, 8, std::string("the delivered byte count"));
    check_equal(last_rate, 48000, std::string("the delivered rate"));
    check_equal(last_channels, 2, std::string("the delivered channel count"));

    session.deliver(payload, 0, 48000, 2);
    check_equal(buffers, 1, std::string("a zero-byte buffer is ignored"));
    session.deliver(nullptr, 8, 48000, 2);
    check_equal(buffers, 1, std::string("a null buffer is ignored"));

    session.stop();
    session.deliver(payload, 8, 48000, 2);
    check_equal(buffers, 1, std::string("a buffer after stop is ignored"));
}

void a_second_session_cannot_hijack_the_callback()
{
    FakeLibrary& fake = FakeLibrary::instance();
    fake = FakeLibrary{};
    voicetyper::platform::McWasapiSession first(fake.api(), 48000, 2);
    voicetyper::platform::McWasapiSession second(fake.api(), 48000, 2);
    static_cast<void>(first.start([](const void*, int, int, int) {}));
    const auto status = second.start([](const void*, int, int, int) {});
    check(!status.is_ok(), "a second live session is refused");
    check_equal(static_cast<int>(status.error().code()),
        static_cast<int>(voicetyper::domain::ErrorCode::invalid_state), std::string("the error code"));
    check_equal(fake.start_calls, 1, std::string("mc_start calls while one session is live"));

    first.stop();
    const auto again = second.start([](const void*, int, int, int) {});
    check(again.is_ok(), "a session can start once the previous one stopped");
    check_equal(fake.start_calls, 2, std::string("mc_start calls after the hand-over"));
    second.stop();
}

} // namespace

int main()
{
    std::printf("mc-wasapi-contract: the mc_wasapi.dll capture path\n");
    a_missing_library_fails_without_calling_anything();
    a_successful_start_and_stop_bookkeeping();
    a_refused_start_reports_the_library_code();
    buffers_reach_the_sink_and_stray_ones_are_ignored();
    a_second_session_cannot_hijack_the_callback();
    if (failures == 0) {
        std::printf("mc-wasapi-contract: OK (%d checks)\n", checks);
        return 0;
    }
    std::printf("mc-wasapi-contract: %d checks, %d failures\n", checks, failures);
    return 1;
}
