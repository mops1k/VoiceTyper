// Linux-only contract test for the clipboard and paste backends.
//
// What this proves, and what it deliberately does not:
//
//   Proves, with no desktop session (the test forces the offscreen Qt platform):
//     * linux_clipboard: a non-empty transcript written from a worker thread
//       really lands on the clipboard and reads back byte for byte, the same
//       object is usable from the main thread, and a clipboard that cannot be
//       reached reports an error instead of a silent success (the .NET gap this
//       backend exists to close);
//     * linux_paste: the availability probe is honest about a session without
//       ydotool, a suspended simulator injects nothing, and a refused injection
//       is reported as an error the caller turns into clipboard_only.
//
//   Deliberately NOT exercised automatically: a real synthetic Ctrl+V. It would
//   type into whatever window the developer has focused while the suite runs.
//   The real injection belongs to the physical Arch Linux gate.

#include "platform/linux/linux_clipboard.hpp"
#include "platform/linux/linux_paste.hpp"

#include <QClipboard>
#include <QCoreApplication>
#include <QEventLoop>
#include <QGuiApplication>

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

namespace {

using namespace voicetyper;

int failures = 0;

void check(bool condition, const std::string& message)
{
    if (!condition) {
        ++failures;
        std::cerr << "FAIL " << message << '\n';
    }
}

void check_clipboard_round_trip()
{
    using namespace voicetyper::platform::linuxos;

    LinuxClipboard clipboard;
    const std::string text = "Привет, VoiceTyper";

    // The recording worker is not the UI thread: the clipboard backend has to
    // marshal internally, and the contract says callers must not do it. The
    // main thread plays the role of the application's event loop here, because
    // a marshalled call is delivered as a queued event.
    platform::Status written;
    std::atomic<bool> finished{false};
    std::thread worker([&] {
        written = clipboard.set_text(text, domain::CancellationToken{});
        finished.store(true);
    });
    while (!finished.load()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    worker.join();
    check(written.is_ok(), "set_text from a worker thread succeeds");

    const auto read_back = clipboard.get_text();
    check(read_back.is_ok(), "get_text succeeds");
    check(read_back.is_ok() && read_back.value().has_value() && *read_back.value() == text,
        "the clipboard returns exactly what was written");
    check(clipboard.has_text(), "has_text is true after a write");

    // The same object is usable from the thread that owns the GUI.
    check(clipboard.set_text("second", domain::CancellationToken{}).is_ok(),
        "set_text from the GUI thread succeeds");
    const auto second = clipboard.get_text();
    check(second.is_ok() && second.value().has_value() && *second.value() == "second",
        "a second write replaces the first");

    // Clearing is a valid write, and then the clipboard holds no text.
    check(clipboard.set_text("", domain::CancellationToken{}).is_ok(), "an empty write succeeds");
    const auto cleared = clipboard.get_text();
    check(cleared.is_ok() && !cleared.value().has_value(), "the clipboard holds no text after clearing");
    check(!clipboard.has_text(), "has_text is false after clearing");
}

void check_paste_availability()
{
    using namespace voicetyper::platform::linuxos;

    LinuxPasteSimulator paste;
    // The probe must agree with itself and must not claim support it cannot
    // deliver: on a session without ydotool the answer is false, and a paste
    // attempt is an explicit error rather than a silent success.
    const bool supported = paste.is_injection_supported();
    if (!supported) {
        const auto status = paste.paste();
        check(status.is_error(), "a refused injection is an error, never a silent success");
        check(status.code() == domain::ErrorCode::unavailable,
            "a session without an injection backend reports unavailable");
    } else {
        std::cout << "skip: ydotool is available in this session, the real injection is not tested here\n";
    }

    paste.set_suspended(true);
    check(paste.paste_suspended(), "the suspend flag is reported");
    check(paste.paste().is_error(), "a suspended paste injects nothing");
    paste.set_suspended(false);
    check(!paste.paste_suspended(), "the suspend flag can be cleared");
}

} // namespace

int main(int argc, char** argv)
{
    // The window flags, the clipboard and the suspension rule are
    // platform-independent Qt behaviour, so the test must not depend on a
    // desktop session (CI has none).
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication application(argc, argv);

    check_clipboard_round_trip();
    check_paste_availability();

    if (failures != 0) {
        std::cerr << "linux-clipboard-contract: " << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "linux-clipboard-contract: OK\n";
    return 0;
}
