#pragma once

// System clipboard contract.
//
// Evidence: docs/migration/cpp/compatibility-contracts.md §3 "Clipboard and
// paste"; .NET reference VoiceTyper.Core/Abstractions/IClipboardWriter.cs and
// VoiceTyper.App/Services/AvaloniaClipboardWriter.cs.
//
// Frozen observable contract:
//   * The clipboard is the source of truth for a recognized transcript. Every
//     nonempty transcript is written to the clipboard regardless of whether the
//     auto-paste succeeds (compatibility-contracts.md §3, "clipboard must remain
//     authoritative").
//   * An empty or whitespace-only transcript must not touch the clipboard at
//     all. That decision belongs to the caller (text output), not to the
//     clipboard backend.
//   * The Windows writer is allowed up to 5 attempts with a 120 ms delay,
//     because another process may hold the clipboard open.
//
// Intent-parity decision recorded here: the current Avalonia writer can return
// silently when no main window exists, which makes a lost transcript invisible.
// The C++ contract reports ErrorCode::unavailable in that case instead, so the
// caller can log it and tell the user. This is a deliberate change from a .NET
// no-op gap and requires a regression test in the Phase D clipboard work.
//
// Thread affinity: set_text() must be callable from a worker thread. Backends
// that need a UI thread (Windows) marshal internally; callers must NOT assume
// they need to do it. Implementations must not block the UI thread and must not
// let an exception escape.
//
// Ownership: text is passed as a view and must be copied before returning.

#include "domain/cancellation.hpp"
#include "domain/error.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace voicetyper::platform {

using domain::CancellationToken;
using domain::ErrorCode;
using domain::Result;
using domain::Status;

/// Maximum attempts the Windows backend makes to acquire the clipboard.
inline constexpr int kClipboardMaxAttempts = 5;
/// Delay between clipboard acquisition attempts, milliseconds.
inline constexpr int kClipboardRetryDelayMs = 120;

class Clipboard {
public:
    virtual ~Clipboard() = default;

    Clipboard(const Clipboard&) = delete;
    Clipboard& operator=(const Clipboard&) = delete;
    Clipboard(Clipboard&&) = delete;
    Clipboard& operator=(Clipboard&&) = delete;

    /// Writes `text` to the system clipboard.
    ///
    /// Failure codes: permission_denied (clipboard locked by another process
    /// after all retries, or blocked by OS policy), unavailable (no clipboard
    /// owner / no UI thread to marshal to), cancelled.
    /// An empty string is a valid write only if the caller decided to clear the
    /// clipboard; the text-output layer never calls this with empty text.
    virtual Status set_text(std::string_view text, const CancellationToken& cancellation) = 0;

    /// Reads the current clipboard text, when the clipboard holds text.
    /// Returns nullopt (a success) when the clipboard holds no text.
    [[nodiscard]] virtual Result<std::optional<std::string>> get_text() const = 0;

    /// True when the clipboard currently holds text. Cheap probe for the UI.
    [[nodiscard]] virtual bool has_text() const = 0;

protected:
    Clipboard() = default;
};

} // namespace voicetyper::platform
