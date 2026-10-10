#include "platform/linux/linux_paste.hpp"

#include <QProcess>
#include <QStandardPaths>
#include <QString>
#include <QStringList>

#include <array>
#include <filesystem>
#include <string>
#include <utility>

namespace voicetyper::platform::linuxos {
namespace {

/// Linux input event codes (linux/input-event-codes.h): KEY_LEFTCTRL and KEY_V.
/// ydotool's `key` subcommand takes exactly these codes with a `:1` press or
/// `:0` release suffix, so Ctrl+V is four events.
constexpr std::array<const char*, 4> kCtrlVSequence{"29:1", "47:1", "47:0", "29:0"};

std::string ydotool_executable()
{
    const QString override_path = qEnvironmentVariable(kYdotoolExecutableVariable.data());
    if (!override_path.isEmpty()) {
        return override_path.toStdString();
    }
    const QString found = QStandardPaths::findExecutable(QStringLiteral("ydotool"));
    return found.toStdString();
}

/// The daemon socket path. ydotool only works through ydotoold, and the daemon
/// is what actually owns the uinput device, so its socket is the honest test of
/// "can this session inject anything at all".
std::filesystem::path daemon_socket_path()
{
    const QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (runtime.isEmpty()) {
        return {};
    }
    return std::filesystem::path(runtime.toStdString()) / std::string(kYdotoolSocketName);
}

} // namespace

LinuxPasteSimulator::~LinuxPasteSimulator() = default;

bool LinuxPasteSimulator::is_injection_supported() const noexcept
{
    try {
        if (ydotool_executable().empty()) {
            diagnostics_ = "ydotool is not installed (or not on PATH)";
            return false;
        }
        const auto socket = daemon_socket_path();
        if (socket.empty()) {
            diagnostics_ = "XDG_RUNTIME_DIR is not set, so the ydotool daemon socket is unknown";
            return false;
        }
        std::error_code error;
        if (!std::filesystem::exists(socket, error) || error) {
            diagnostics_ = "the ydotool daemon is not running (no " + socket.string()
                + "; start it with systemctl --user start ydotool.service)";
            return false;
        }
        diagnostics_.clear();
        return true;
    } catch (...) {
        diagnostics_ = "the injection probe failed";
        return false;
    }
}

Status LinuxPasteSimulator::paste()
{
    if (suspended_.load(std::memory_order_acquire)) {
        // Reported as unavailable with an explicit message; the caller renders
        // it as clipboard_only, which is the state the contract asks for during
        // hotkey capture.
        return Status::failure(ErrorCode::unavailable, "injection suspended for hotkey capture");
    }
    if (!is_injection_supported()) {
        return Status::failure(ErrorCode::unavailable, diagnostics_);
    }

    const std::string executable = ydotool_executable();
    QStringList arguments;
    arguments << QStringLiteral("key");
    for (const char* event : kCtrlVSequence) {
        arguments << QString::fromLatin1(event);
    }

    QProcess process;
    process.setProgram(QString::fromStdString(executable));
    process.setArguments(arguments);
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start();
    if (!process.waitForStarted(1500)) {
        diagnostics_ = "could not start " + executable;
        return Status::failure(ErrorCode::unavailable, diagnostics_);
    }
    if (!process.waitForFinished(1500)) {
        process.kill();
        process.waitForFinished(500);
        diagnostics_ = executable + " did not finish within 1500 ms";
        return Status::failure(ErrorCode::unavailable, diagnostics_);
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        const QString output = QString::fromUtf8(process.readAll()).trimmed();
        diagnostics_ = executable + " failed (exit " + QString::number(process.exitCode()).toStdString()
            + (output.isEmpty() ? std::string() : ": " + output.toStdString()) + ")";
        // A refused uinput access is the case the user can act on; anything else
        // is reported as an unavailable backend rather than a lost transcript.
        if (output.contains(QStringLiteral("Permission denied"), Qt::CaseInsensitive)
            || output.contains(QStringLiteral("/dev/uinput"), Qt::CaseInsensitive)) {
            return Status::failure(ErrorCode::permission_denied, diagnostics_);
        }
        return Status::failure(ErrorCode::unavailable, diagnostics_);
    }
    diagnostics_.clear();
    return Status::success();
}

void LinuxPasteSimulator::set_suspended(bool suspended) noexcept
{
    suspended_.store(suspended, std::memory_order_release);
}

bool LinuxPasteSimulator::paste_suspended() const noexcept
{
    return suspended_.load(std::memory_order_acquire);
}

std::string LinuxPasteSimulator::diagnostics() const
{
    return diagnostics_;
}

std::string LinuxPasteSimulator::command_description() const
{
    std::string text = ydotool_executable();
    if (text.empty()) {
        text = "(ydotool not found)";
    }
    text += " key 29:1 47:1 47:0 29:0";
    return text;
}

} // namespace voicetyper::platform::linuxos
