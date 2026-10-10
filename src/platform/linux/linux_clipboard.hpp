#pragma once

// Linux implementation of the frozen platform::Clipboard contract.
//
// Evidence and contract:
//   * src/platform/api/clipboard.hpp - a non-empty transcript always reaches the
//     clipboard, an empty one is the caller's decision, and a backend that
//     cannot deliver must report it instead of returning a silent success (the
//     .NET Avalonia writer's no-op gap).
//   * src/domain/text_output.* - RetryingClipboard decorates this backend and
//     TextOutputService treats a failed clipboard write as clipboard_failed.
//
// Why Qt and not a helper binary: the clipboard on Wayland belongs to the
// focused client, and the only owner this process can rely on is its own Qt
// session (QGuiApplication::clipboard()). Shelling out to xclip/wl-copy would
// add an external dependency that is not installed on the target machine
// (verified 2026-10-08: neither wl-copy nor xclip is present), and a helper
// process would lose the selection as soon as it exits on some compositors.
//
// Thread affinity: QClipboard may only be touched from the thread that owns the
// GUI, while the contract requires set_text() to be callable from a worker
// thread. Every method therefore marshals to the GUI thread and waits with a
// bounded timeout; a call already on the GUI thread runs directly, because a
// blocking queued call from that thread would deadlock the process. A call with
// no QGuiApplication at all is unavailable - never a silent success.
//
// Ownership: the object owns nothing; the clipboard belongs to QGuiApplication,
// which must outlive it.

#include "domain/cancellation.hpp"
#include "domain/error.hpp"
#include "platform/api/clipboard.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

namespace voicetyper::platform::linuxos {

using domain::CancellationToken;
using domain::ErrorCode;
using domain::Result;
using domain::Status;

/// How long a marshalled clipboard call waits for the GUI thread. The frozen
/// retry policy (kClipboardMaxAttempts x kClipboardRetryDelayMs) is applied by
/// the caller on top of this, so the two do not multiply into an unbounded wait.
inline constexpr std::chrono::milliseconds kClipboardMarshalTimeout{1500};

class LinuxClipboard final : public platform::Clipboard {
public:
    /// Uses the clipboard of the current QGuiApplication. Construction without
    /// one is allowed; every call then reports unavailable.
    LinuxClipboard();
    ~LinuxClipboard() override;

    LinuxClipboard(const LinuxClipboard&) = delete;
    LinuxClipboard& operator=(const LinuxClipboard&) = delete;
    LinuxClipboard(LinuxClipboard&&) = delete;
    LinuxClipboard& operator=(LinuxClipboard&&) = delete;

    /// Writes `text` to the system clipboard. Failure codes: unavailable (no GUI
    /// application, or the GUI thread did not answer in time),
    /// permission_denied (the compositor refused the selection), cancelled.
    Status set_text(std::string_view text, const CancellationToken& cancellation) override;

    /// Reads the current clipboard text. nullopt (a success) means the clipboard
    /// holds no text at all.
    [[nodiscard]] Result<std::optional<std::string>> get_text() const override;

    /// Cheap probe for the UI.
    [[nodiscard]] bool has_text() const override;
};

} // namespace voicetyper::platform::linuxos
