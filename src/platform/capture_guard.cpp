#include "platform/api/capture_guard.hpp"

#include <utility>

#if defined(_WIN32)
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <thread>
#include <windows.h>
// dbghelp has to come after windows.h. It gives the crash handler both the symbolised
// stack (SymFromAddr/StackWalk64) and the minidump (MiniDumpWriteDump).
#include <dbghelp.h>
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

/// Both crash files live in the directory the launcher already uses for logs, next to
/// the executable otherwise. The path is built with Win32 calls only: this code runs
/// when the heap may already be damaged.
void crash_file_path(const wchar_t* name, wchar_t* out, DWORD count)
{
    DWORD length = ::GetEnvironmentVariableW(L"VOICETYPER_LOG_DIR", out, count);
    if (length == 0 || length >= count) {
        length = ::GetModuleFileNameW(nullptr, out, count);
        while (length > 0 && out[length - 1] != L'\\') {
            --length;
        }
    }
    if (length > 0 && out[length - 1] != L'\\') {
        out[length++] = L'\\';
        out[length] = L'\0';
    }
    if (length + 32 >= count) {
        out[0] = L'\0';
        return;
    }
    ::swprintf_s(out + length, count - length, L"%s-%lu.txt", name, ::GetCurrentProcessId());
}

/// "module.dll+0x1234" - the fallback when the symbol table cannot name a frame.
void append_module_offset(HANDLE process, DWORD64 address, char* out, std::size_t size)
{
    IMAGEHLP_MODULE64 module{};
    module.SizeOfStruct = sizeof(module);
    if (::SymGetModuleInfo64(process, address, &module) != FALSE) {
        const char* image = module.ImageName;
        const char* slash = std::strrchr(image, '\\');
        std::snprintf(out, size, "%s+0x%llx", slash != nullptr ? slash + 1 : image,
            static_cast<unsigned long long>(address - module.BaseOfImage));
        return;
    }
    std::snprintf(out, size, "0x%llx", static_cast<unsigned long long>(address));
}

void write_crash_report(EXCEPTION_POINTERS* info)
{
    wchar_t path[MAX_PATH]{};
    crash_file_path(L"crash", path, MAX_PATH);
    if (path[0] == L'\0') {
        return;
    }
    HANDLE file = ::CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    char line[1024];
    const DWORD code = (info != nullptr && info->ExceptionRecord != nullptr)
        ? info->ExceptionRecord->ExceptionCode
        : 0;
    const void* address = (info != nullptr && info->ExceptionRecord != nullptr)
        ? info->ExceptionRecord->ExceptionAddress
        : nullptr;
    int length = std::snprintf(line, sizeof(line),
        "=== crash pid=%lu thread=%lu code=0x%08lx address=%p ===\r\n",
        ::GetCurrentProcessId(), ::GetCurrentThreadId(), code, address);
    DWORD written = 0;
    ::WriteFile(file, line, static_cast<DWORD>(length), &written, nullptr);

    HANDLE process = ::GetCurrentProcess();
    ::SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_NO_PROMPTS);
    const bool have_symbols = ::SymInitialize(process, nullptr, TRUE) != FALSE;
    if (info != nullptr && info->ContextRecord != nullptr && have_symbols) {
        CONTEXT context = *info->ContextRecord;
        STACKFRAME64 frame{};
        frame.AddrPC.Offset = context.Rip;
        frame.AddrPC.Mode = AddrModeFlat;
        frame.AddrFrame.Offset = context.Rbp;
        frame.AddrFrame.Mode = AddrModeFlat;
        frame.AddrStack.Offset = context.Rsp;
        frame.AddrStack.Mode = AddrModeFlat;
        alignas(SYMBOL_INFO) char symbol_buffer[sizeof(SYMBOL_INFO) + 256] = {};
        for (int index = 0; index < 40; ++index) {
            if (::StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, ::GetCurrentThread(), &frame,
                    &context, nullptr, ::SymFunctionTableAccess64, ::SymGetModuleBase64, nullptr) == FALSE) {
                break;
            }
            if (frame.AddrPC.Offset == 0) {
                break;
            }
            auto* symbol = reinterpret_cast<SYMBOL_INFO*>(symbol_buffer);
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = 255;
            DWORD64 displacement = 0;
            char tail[320]{};
            if (::SymFromAddr(process, frame.AddrPC.Offset, &displacement, symbol) != FALSE) {
                IMAGEHLP_LINE64 location{};
                location.SizeOfStruct = sizeof(location);
                DWORD line_displacement = 0;
                if (::SymGetLineFromAddr64(process, frame.AddrPC.Offset, &line_displacement, &location) != FALSE) {
                    std::snprintf(tail, sizeof(tail), "%s+0x%llx  (%s:%lu)", symbol->Name,
                        static_cast<unsigned long long>(displacement), location.FileName,
                        static_cast<unsigned long>(location.LineNumber));
                } else {
                    std::snprintf(tail, sizeof(tail), "%s+0x%llx", symbol->Name,
                        static_cast<unsigned long long>(displacement));
                }
            } else {
                append_module_offset(process, frame.AddrPC.Offset, tail, sizeof(tail));
            }
            length = std::snprintf(line, sizeof(line), "  #%02d  %s\r\n", index, tail);
            ::WriteFile(file, line, static_cast<DWORD>(length), &written, nullptr);
        }
        ::SymCleanup(process);
    }
    ::CloseHandle(file);

    // A minidump next to the text: the next crash stays fully analysable, which is what
    // the first one was missing.
    wchar_t dump_path[MAX_PATH]{};
    crash_file_path(L"crash", dump_path, MAX_PATH);
    if (dump_path[0] != L'\0') {
        const std::size_t used = std::wcslen(dump_path);
        if (used > 4) {
            std::wmemcpy(dump_path + used - 4, L".dmp", 4);
        }
        HANDLE dump = ::CreateFileW(dump_path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (dump != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION where{};
            where.ThreadId = ::GetCurrentThreadId();
            where.ExceptionPointers = info;
            where.ClientPointers = FALSE;
            ::MiniDumpWriteDump(::GetCurrentProcess(), ::GetCurrentProcessId(), dump,
                MiniDumpNormal, info != nullptr ? &where : nullptr, nullptr, nullptr);
            ::CloseHandle(dump);
        }
    }
}

/// The stack of the calling thread, written next to the log. Qt says "cannot create
/// children for a parent that is in a different thread" without saying who did it, and a
/// defect of exactly that kind was living in the window (Alexander, 08.10.2026).
void write_thread_stack(const char* reason)
{
    wchar_t path[MAX_PATH]{};
    crash_file_path(L"wrong-thread", path, MAX_PATH);
    if (path[0] == L'\0') {
        return;
    }
    HANDLE file = ::CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD written = 0;
    char line[1024];
    int length = std::snprintf(line, sizeof(line), "=== wrong thread (%s) pid=%lu thread=%lu ===\r\n",
        reason, ::GetCurrentProcessId(), ::GetCurrentThreadId());
    ::WriteFile(file, line, static_cast<DWORD>(length), &written, nullptr);

    CONTEXT context{};
    ::RtlCaptureContext(&context);
    HANDLE process = ::GetCurrentProcess();
    ::SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_NO_PROMPTS);
    const bool have_symbols = ::SymInitialize(process, nullptr, TRUE) != FALSE;
    STACKFRAME64 frame{};
    frame.AddrPC.Offset = context.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = context.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;
    alignas(SYMBOL_INFO) char symbol_buffer[sizeof(SYMBOL_INFO) + 256] = {};
    for (int index = 0; index < 32; ++index) {
        if (::StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, ::GetCurrentThread(), &frame, &context,
                nullptr, ::SymFunctionTableAccess64, ::SymGetModuleBase64, nullptr) == FALSE
            || frame.AddrPC.Offset == 0) {
            break;
        }
        char tail[320]{};
        if (have_symbols) {
            auto* symbol = reinterpret_cast<SYMBOL_INFO*>(symbol_buffer);
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = 255;
            DWORD64 displacement = 0;
            if (::SymFromAddr(process, frame.AddrPC.Offset, &displacement, symbol) != FALSE) {
                std::snprintf(tail, sizeof(tail), "%s+0x%llx", symbol->Name,
                    static_cast<unsigned long long>(displacement));
            }
        }
        if (tail[0] == '\0') {
            append_module_offset(process, frame.AddrPC.Offset, tail, sizeof(tail));
        }
        length = std::snprintf(line, sizeof(line), "  #%02d %s\r\n", index, tail);
        ::WriteFile(file, line, static_cast<DWORD>(length), &written, nullptr);
    }
    if (have_symbols) {
        ::SymCleanup(process);
    }
    ::CloseHandle(file);
}

LONG WINAPI crash_release_filter(EXCEPTION_POINTERS* info)
{
    if (g_guard != nullptr) {
        g_guard->release_now();
    }
    // The report is written before WER takes over: the microphone is released by now, and
    // a heap that is already damaged may not survive much longer.
    write_crash_report(info);
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

void log_stack_trace(const char* reason)
{
    write_thread_stack(reason);
}

void install_crash_release_hook(CaptureGuard& guard)
{
    g_guard = &guard;
    if (g_previous_filter == nullptr) {
        g_previous_filter = ::SetUnhandledExceptionFilter(&crash_release_filter);
    }
    if (g_previous_terminate == nullptr) {
        g_previous_terminate = std::set_terminate(&crash_release_terminate);
    }
    // Self-test (VOICETYPER_CRASH_SELFTEST=1): raise a real access violation in a
    // separate thread so the report above can be proven end to end rather than trusted.
    if (std::getenv("VOICETYPER_CRASH_SELFTEST") != nullptr) {
        std::thread([] {
            std::this_thread::sleep_for(std::chrono::seconds(3));
            ::RaiseException(EXCEPTION_ACCESS_VIOLATION, EXCEPTION_NONCONTINUABLE, 0, nullptr);
        }).detach();
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
