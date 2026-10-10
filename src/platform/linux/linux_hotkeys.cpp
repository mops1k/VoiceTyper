#include "platform/linux/linux_hotkeys.hpp"

#include "platform/linux/linux_keymap.hpp"

#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <string>
#include <utility>
#include <vector>

namespace voicetyper::platform::linuxos {
namespace {

constexpr int kMaxEventNodes = 32;
constexpr auto kPollSlice = std::chrono::milliseconds(50);

/// True when the node looks like a keyboard: it reports key events and has the
/// letter keys and space. A power button, a mouse and a lid switch all report
/// EV_KEY but none of them has KEY_A..KEY_Z.
bool is_keyboard(int descriptor)
{
    unsigned long event_bits = 0;
    if (::ioctl(descriptor, EVIOCGBIT(0, sizeof(event_bits)), &event_bits) < 0) {
        return false;
    }
    if ((event_bits & (1UL << EV_KEY)) == 0) {
        return false;
    }
    std::vector<unsigned long> key_bits((KEY_MAX / (8 * sizeof(unsigned long))) + 1, 0);
    if (::ioctl(descriptor, EVIOCGBIT(EV_KEY, key_bits.size() * sizeof(unsigned long)), key_bits.data()) < 0) {
        return false;
    }
    const auto has_key = [&key_bits](int code) {
        const auto index = static_cast<std::size_t>(code / (8 * sizeof(unsigned long)));
        const auto bit = static_cast<unsigned long>(code % (8 * sizeof(unsigned long)));
        return index < key_bits.size() && (key_bits[index] & (1UL << bit)) != 0;
    };
    return has_key(KEY_A) && has_key(KEY_Z) && has_key(KEY_SPACE);
}

/// Opens every readable keyboard node. `denied` reports that nodes existed but
/// could not be opened, which is the state a user who is not in the `input`
/// group is in.
std::vector<int> open_keyboards(bool& saw_node, bool& denied, std::string& diagnostics)
{
    std::vector<int> descriptors;
    for (int index = 0; index < kMaxEventNodes; ++index) {
        const std::string path = "/dev/input/event" + std::to_string(index);
        const int descriptor = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (descriptor < 0) {
            if (errno == EACCES || errno == EPERM) {
                saw_node = true;
                denied = true;
                if (!diagnostics.empty()) {
                    diagnostics += ", ";
                }
                diagnostics += path + " is not readable";
            }
            continue;
        }
        if (!is_keyboard(descriptor)) {
            ::close(descriptor);
            continue;
        }
        saw_node = true;
        descriptors.push_back(descriptor);
    }
    return descriptors;
}

/// The modifier bits currently held, from the raw event stream.
std::uint8_t apply_modifier_event(std::uint8_t state, std::int32_t code, int value)
{
    const std::uint8_t bits = modifier_bits_for_key_code(code);
    if (bits == 0) {
        return state;
    }
    if (value == 1) {
        return static_cast<std::uint8_t>(state | bits);
    }
    if (value == 0) {
        return static_cast<std::uint8_t>(state & static_cast<std::uint8_t>(~bits));
    }
    return state;
}

} // namespace

// ---------------------------------------------------------------------------
// LinuxHotkeyService
// ---------------------------------------------------------------------------

struct LinuxHotkeyService::Impl {
    std::mutex mutex;
    platform::HotkeyEventSink sink;
    HotkeyGesture record;
    HotkeyGesture cancel;
    std::int32_t record_code = 0;
    std::int32_t cancel_code = 0;
    std::vector<int> descriptors;
    std::thread reader;
    std::atomic_bool stopping{false};
    std::atomic_bool running{false};
    std::string diagnostics;
    std::uint8_t modifier_state = 0;

    void reader_loop();
};

void LinuxHotkeyService::Impl::reader_loop()
{
    std::vector<pollfd> poll_fds;
    poll_fds.reserve(descriptors.size());
    for (const int descriptor : descriptors) {
        poll_fds.push_back(pollfd{descriptor, POLLIN, 0});
    }

    while (!stopping.load(std::memory_order_acquire)) {
        const int ready = ::poll(poll_fds.data(), poll_fds.size(), static_cast<int>(kPollSlice.count()));
        if (ready <= 0) {
            continue;
        }
        for (auto& entry : poll_fds) {
            if ((entry.revents & POLLIN) == 0) {
                continue;
            }
            input_event event{};
            const ssize_t bytes = ::read(entry.fd, &event, sizeof(event));
            if (bytes != static_cast<ssize_t>(sizeof(event)) || event.type != EV_KEY) {
                continue;
            }

            platform::HotkeyEventSink sink;
            platform::HotkeyAction action = platform::HotkeyAction::record_pressed;
            bool fire = false;

            {
                std::lock_guard<std::mutex> lock(mutex);
                if (is_modifier_key_code(event.code)) {
                    modifier_state = apply_modifier_event(modifier_state, event.code, event.value);
                    continue;
                }
                if (event.value == 1) {
                    if (record_code != 0 && event.code == record_code
                        && static_cast<std::uint8_t>(record.modifiers) == modifier_state) {
                        action = platform::HotkeyAction::record_pressed;
                        fire = true;
                    } else if (cancel_code != 0 && event.code == cancel_code
                        && static_cast<std::uint8_t>(cancel.modifiers) == modifier_state) {
                        action = platform::HotkeyAction::cancel_pressed;
                        fire = true;
                    }
                } else if (event.value == 0 && record_code != 0 && event.code == record_code) {
                    // The release edge is what push-to-talk needs, and it is
                    // reported even when a modifier was released first.
                    action = platform::HotkeyAction::record_released;
                    fire = true;
                }
                if (fire) {
                    sink = this->sink;
                }
            }

            if (fire && sink) {
                try {
                    sink(action);
                } catch (...) {
                    // An exception must never escape into the reader thread.
                }
            }
        }
    }
}

LinuxHotkeyService::LinuxHotkeyService()
    : impl_(std::make_unique<Impl>())
{
}

LinuxHotkeyService::~LinuxHotkeyService()
{
    static_cast<void>(unregister_all());
}

Status LinuxHotkeyService::set_event_sink(platform::HotkeyEventSink sink)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->sink = std::move(sink);
    return Status::success();
}

Result<platform::HotkeyRegistrationReport> LinuxHotkeyService::apply_settings(const AppSettings& settings)
{
    platform::HotkeyRegistrationReport report;
    platform::HotkeyRegistration record_entry;
    platform::HotkeyRegistration cancel_entry;
    record_entry.action = platform::HotkeyAction::record_pressed;
    cancel_entry.action = platform::HotkeyAction::cancel_pressed;

    const auto record_gesture = domain::parse_hotkey(settings.record_hotkey);
    const auto cancel_gesture = domain::parse_hotkey(settings.cancel_hotkey);

    if (record_gesture.is_ok()) {
        record_entry.gesture = record_gesture.value();
    } else {
        record_entry.error = record_gesture.error().message();
    }
    if (cancel_gesture.is_ok()) {
        cancel_entry.gesture = cancel_gesture.value();
    } else {
        cancel_entry.error = cancel_gesture.error().message();
    }

    // Release whatever is registered before the devices are scanned again: a
    // keyboard plugged in since the last call is picked up here.
    static_cast<void>(unregister_all());

    bool saw_node = false;
    bool denied = false;
    std::string diagnostics;
    std::vector<int> descriptors = open_keyboards(saw_node, denied, diagnostics);

    if (descriptors.empty()) {
        const std::string reason = denied
            ? "the input devices are not readable (" + diagnostics
                + "); add this user to the input group or install a udev rule"
            : "no keyboard input device was found under /dev/input";
        if (record_gesture.is_ok()) {
            record_entry.registered = false;
            record_entry.error = reason;
        }
        if (cancel_gesture.is_ok()) {
            cancel_entry.registered = false;
            cancel_entry.error = reason;
        }
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            impl_->diagnostics = reason;
        }
        report.record.push_back(std::move(record_entry));
        report.cancel.push_back(std::move(cancel_entry));
        return report;
    }

    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->descriptors = std::move(descriptors);
        impl_->diagnostics = "keyboards opened: " + std::to_string(impl_->descriptors.size());
        if (record_gesture.is_ok()) {
            impl_->record = record_gesture.value();
            impl_->record_code = hotkey_key_code(impl_->record.key);
            record_entry.registered = impl_->record_code != 0;
            record_entry.native_key_code = impl_->record_code;
            if (!record_entry.registered) {
                record_entry.error = "the key name is not known on Linux: " + impl_->record.key;
            }
        }
        if (cancel_gesture.is_ok()) {
            impl_->cancel = cancel_gesture.value();
            impl_->cancel_code = hotkey_key_code(impl_->cancel.key);
            cancel_entry.registered = impl_->cancel_code != 0;
            cancel_entry.native_key_code = impl_->cancel_code;
            if (!cancel_entry.registered) {
                cancel_entry.error = "the key name is not known on Linux: " + impl_->cancel.key;
            }
        }
        impl_->stopping.store(false, std::memory_order_release);
    }

    impl_->reader = std::thread([this] { impl_->reader_loop(); });
    impl_->running.store(true, std::memory_order_release);

    report.record.push_back(std::move(record_entry));
    report.cancel.push_back(std::move(cancel_entry));
    return report;
}

Status LinuxHotkeyService::unregister_all()
{
    if (impl_->running.load(std::memory_order_acquire)) {
        impl_->stopping.store(true, std::memory_order_release);
        if (impl_->reader.joinable()) {
            impl_->reader.join();
        }
        impl_->running.store(false, std::memory_order_release);
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (const int descriptor : impl_->descriptors) {
        ::close(descriptor);
    }
    impl_->descriptors.clear();
    impl_->record_code = 0;
    impl_->cancel_code = 0;
    impl_->record = HotkeyGesture{};
    impl_->cancel = HotkeyGesture{};
    impl_->modifier_state = 0;
    return Status::success();
}

std::int32_t LinuxHotkeyService::record_key_code() const noexcept
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->record_code;
}

bool LinuxHotkeyService::input_devices_available() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return !impl_->descriptors.empty();
}

HotkeyCapability LinuxHotkeyService::capability() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->descriptors.empty() ? HotkeyCapability::none : HotkeyCapability::evdev;
}

std::string LinuxHotkeyService::diagnostics() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->diagnostics;
}

// ---------------------------------------------------------------------------
// HotkeyCaptureHook
// ---------------------------------------------------------------------------

struct HotkeyCaptureHook::Impl {
    std::vector<int> descriptors;
    std::string diagnostics;
    bool started = false;
};

HotkeyCaptureHook::HotkeyCaptureHook()
    : impl_(std::make_unique<Impl>())
{
}

HotkeyCaptureHook::~HotkeyCaptureHook()
{
    static_cast<void>(stop());
}

Status HotkeyCaptureHook::start()
{
    static_cast<void>(stop());
    bool saw_node = false;
    bool denied = false;
    std::string diagnostics;
    std::vector<int> descriptors = open_keyboards(saw_node, denied, diagnostics);
    if (descriptors.empty()) {
        impl_->diagnostics = denied
            ? "the input devices are not readable (" + diagnostics + ")"
            : "no keyboard input device was found under /dev/input";
        return Status::failure(denied ? ErrorCode::permission_denied : ErrorCode::unavailable,
            impl_->diagnostics);
    }
    impl_->descriptors = std::move(descriptors);
    impl_->started = true;
    impl_->diagnostics.clear();
    return Status::success();
}

Result<HotkeyGesture> HotkeyCaptureHook::capture_next(const CancellationToken& cancellation)
{
    if (!impl_->started) {
        return Result<HotkeyGesture>::failure(ErrorCode::invalid_state, "the capture hook was not started");
    }

    std::vector<pollfd> poll_fds;
    poll_fds.reserve(impl_->descriptors.size());
    for (const int descriptor : impl_->descriptors) {
        poll_fds.push_back(pollfd{descriptor, POLLIN, 0});
    }

    std::uint8_t modifiers = 0;
    while (true) {
        if (cancellation.is_cancellation_requested()) {
            return Result<HotkeyGesture>::failure(ErrorCode::cancelled, "hotkey capture cancelled");
        }
        const int ready = ::poll(poll_fds.data(), poll_fds.size(), static_cast<int>(kPollSlice.count()));
        if (ready <= 0) {
            continue;
        }
        for (auto& entry : poll_fds) {
            if ((entry.revents & POLLIN) == 0) {
                continue;
            }
            input_event event{};
            const ssize_t bytes = ::read(entry.fd, &event, sizeof(event));
            if (bytes != static_cast<ssize_t>(sizeof(event)) || event.type != EV_KEY) {
                continue;
            }
            if (is_modifier_key_code(event.code)) {
                modifiers = apply_modifier_event(modifiers, event.code, event.value);
                continue;
            }
            if (event.value != 1) {
                continue;
            }
            // Escape with no modifier cancels, exactly like the .NET hook.
            if (event.code == 1 && modifiers == 0) {
                return Result<HotkeyGesture>::failure(ErrorCode::cancelled, "hotkey capture cancelled with Escape");
            }
            const std::string_view name = hotkey_key_name(event.code);
            if (name.empty()) {
                continue;
            }
            HotkeyGesture gesture;
            gesture.modifiers = static_cast<domain::HotkeyModifiers>(modifiers);
            gesture.key = std::string(name);
            if (!gesture.is_capturable()) {
                // The frozen capture rule: a modifier is required unless the key
                // is F1..F24, so a bare letter is ignored rather than captured.
                continue;
            }
            return gesture;
        }
    }
}

Status HotkeyCaptureHook::stop()
{
    for (const int descriptor : impl_->descriptors) {
        ::close(descriptor);
    }
    impl_->descriptors.clear();
    impl_->started = false;
    return Status::success();
}

} // namespace voicetyper::platform::linuxos
