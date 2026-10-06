#include "platform/api/capture_guard.hpp"

#include <utility>

#if defined(_WIN32)
#include <csignal>
#include <cstdlib>
#include <exception>
#include <windows.h>
#endif

namespace voicetyper::platform {

void CaptureGuard::arm(std::function<void()> release)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        release_ = std::move(release);
    }
    armed_.store(true);
}

void CaptureGuard::disarm()
{
    // The flag first: a crash hook that runs concurrently sees "not armed" and does
    // nothing, instead of racing with the assignment below.
    armed_.store(false);
    std::lock_guard<std::mutex> lock(mutex_);
    release_ = nullptr;
}

void CaptureGuard::release_now()
{
    // No mutex on this path: it runs from a crash handler, where taking a lock the
    // dying thread already holds would deadlock. The exchange is what makes the
    // release run at most once, however many threads arrive here.
    if (!armed_.exchange(false)) {
        return;
    }
    const std::function<void()> release = release_;
    if (release) {
        release();
    }
}

bool CaptureGuard::armed() const noexcept
{
    return armed_.load();
}

#if defined(_WIN32)

namespace {

/// The guard the crash hooks release. One process has one microphone, so one guard is
/// the honest model; install_crash_release_hook replaces it if it is called again.
CaptureGuard* g_guard = nullptr;
LPTOP_LEVEL_EXCEPTION_FILTER g_previous_filter = nullptr;
std::terminate_handler g_previous_terminate = nullptr;

LONG WINAPI crash_release_filter(EXCEPTION_POINTERS* info)
{
    if (g_guard != nullptr) {
        g_guard->release_now();
    }
    // The previous filter (Windows Error Reporting by default) still runs: releasing
    // the microphone must not swallow the crash report.
    if (g_previous_filter != nullptr) {
        return g_previous_filter(info);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void crash_release_terminate()
{
    if (g_guard != nullptr) {
        g_guard->release_now();
    }
    if (g_previous_terminate != nullptr) {
        g_previous_terminate();
    }
    std::abort();
}

void crash_release_signal(int)
{
    if (g_guard != nullptr) {
        g_guard->release_now();
    }
    // The default disposition runs next: the process must still die.
    std::signal(SIGABRT, SIG_DFL);
    std::signal(SIGSEGV, SIG_DFL);
    std::signal(SIGFPE, SIG_DFL);
    std::signal(SIGILL, SIG_DFL);
    std::raise(SIGABRT);
}

} // namespace

void install_crash_release_hook(CaptureGuard& guard)
{
    g_guard = &guard;
    if (g_previous_filter == nullptr) {
        g_previous_filter = ::SetUnhandledExceptionFilter(&crash_release_filter);
    }
    if (g_previous_terminate == nullptr) {
        g_previous_terminate = std::set_terminate(&crash_release_terminate);
    }
    static bool signals_installed = false;
    if (!signals_installed) {
        std::signal(SIGABRT, &crash_release_signal);
        std::signal(SIGSEGV, &crash_release_signal);
        std::signal(SIGFPE, &crash_release_signal);
        std::signal(SIGILL, &crash_release_signal);
        signals_installed = true;
    }
}

#else

void install_crash_release_hook(CaptureGuard& guard)
{
    // No hooks on this platform; the guard still works through release_now() and its
    // owning adapter releases the device as soon as it is idle.
    static_cast<void>(guard);
}

#endif

} // namespace voicetyper::platform
