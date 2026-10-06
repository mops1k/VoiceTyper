// Contract for the microphone release fuse.
//
// The hooks themselves are platform work; what is pinned here is the logic that has to
// be right for the fuse to be worth anything: exactly one release per armed session,
// nothing at all once the session ended by itself, and no double release when several
// threads race - a crash path can be reached from more than one thread. Written before
// the implementation, per docs/tdd.md (ADR-012).

#include "platform/api/capture_guard.hpp"

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
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

void an_armed_release_runs_exactly_once()
{
    voicetyper::platform::CaptureGuard guard;
    int releases = 0;
    guard.arm([&releases] { ++releases; });
    check_equal(guard.armed(), true, std::string("armed after arm()"));

    guard.release_now();
    check_equal(releases, 1, std::string("releases after release_now()"));
    check_equal(guard.armed(), false, std::string("armed after the release"));

    guard.release_now();
    check_equal(releases, 1, std::string("a second release_now() must not release again"));
}

void a_disarmed_session_is_not_released()
{
    voicetyper::platform::CaptureGuard guard;
    int releases = 0;
    guard.arm([&releases] { ++releases; });
    guard.disarm();
    check_equal(guard.armed(), false, std::string("armed after disarm()"));
    guard.release_now();
    check_equal(releases, 0, std::string("a session that ended by itself is not released"));

    // The guard is reusable: the next dictation arms it again.
    guard.arm([&releases] { ++releases; });
    guard.release_now();
    check_equal(releases, 1, std::string("the guard arms again for the next session"));
}

void an_unarmed_guard_does_nothing()
{
    voicetyper::platform::CaptureGuard guard;
    guard.release_now();
    check_equal(guard.armed(), false, std::string("an untouched guard stays unarmed"));
}

void racing_releases_release_once()
{
    voicetyper::platform::CaptureGuard guard;
    std::atomic<int> releases{0};
    guard.arm([&releases] { releases.fetch_add(1); });

    std::vector<std::thread> threads;
    threads.reserve(8);
    for (int index = 0; index < 8; ++index) {
        threads.emplace_back([&guard] { guard.release_now(); });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    check_equal(releases.load(), 1, std::string("releases under a race"));
}

void rearming_replaces_the_previous_release()
{
    voicetyper::platform::CaptureGuard guard;
    int first = 0;
    int second = 0;
    guard.arm([&first] { ++first; });
    guard.arm([&second] { ++second; });
    guard.release_now();
    check_equal(first, 0, std::string("the replaced release is not called"));
    check_equal(second, 1, std::string("the current release is called"));
}

} // namespace

int main()
{
    std::printf("capture-guard-contract: the microphone release fuse\n");
    an_armed_release_runs_exactly_once();
    a_disarmed_session_is_not_released();
    an_unarmed_guard_does_nothing();
    racing_releases_release_once();
    rearming_replaces_the_previous_release();
    if (failures == 0) {
        std::printf("capture-guard-contract: OK (%d checks)\n", checks);
        return 0;
    }
    std::printf("capture-guard-contract: %d checks, %d failures\n", checks, failures);
    return 1;
}
