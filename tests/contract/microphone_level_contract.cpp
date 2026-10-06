// Contract for the microphone level controller.
//
// The platform port is injected, so this pins the logic that must not depend on a
// device: the range mapping, the refusal to unmute behind the user's back, and the
// behaviour when the platform has no level control at all. Written before the
// implementation, per docs/tdd.md (ADR-012).

#include "platform/api/microphone_level.hpp"

#include <cstdio>
#include <string>

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

/// A stand-in for the platform, recording what the controller asks of it.
class FakePort final : public voicetyper::platform::MicrophoneLevelPort {
public:
    voicetyper::platform::MicrophoneLevelState reported{true, false, 0.5};
    bool fail_write = false;
    int writes = 0;
    double last_level = -1.0;
    bool last_muted = false;

    [[nodiscard]] voicetyper::platform::MicrophoneLevelState read() override { return reported; }

    [[nodiscard]] voicetyper::platform::Status write(double level, bool muted) override
    {
        ++writes;
        last_level = level;
        last_muted = muted;
        if (fail_write) {
            return voicetyper::platform::Status::failure(
                voicetyper::platform::ErrorCode::permission_denied, "the device refused the level");
        }
        reported.level = level;
        reported.muted = muted;
        return voicetyper::platform::Status::success();
    }
};

void the_range_mapping_round_trips()
{
    using voicetyper::platform::clamp_microphone_level;
    using voicetyper::platform::microphone_level_from_percent;
    using voicetyper::platform::microphone_level_percent;

    check_equal(clamp_microphone_level(-1.0), 0.0, std::string("a negative level clamps to zero"));
    check_equal(clamp_microphone_level(2.0), 1.0, std::string("a level above one clamps to one"));
    check_equal(clamp_microphone_level(0.42), 0.42, std::string("a level inside the range is kept"));
    check_equal(microphone_level_percent(0.0), 0, std::string("zero is 0%"));
    check_equal(microphone_level_percent(1.0), 100, std::string("one is 100%"));
    check_equal(microphone_level_percent(0.525), 53, std::string("a fraction rounds to the nearest percent"));
    check_equal(microphone_level_from_percent(150), 1.0, std::string("a percent above 100 clamps"));
    check_equal(microphone_level_from_percent(-20), 0.0, std::string("a percent below 0 clamps"));
    check_equal(microphone_level_from_percent(40), 0.4, std::string("a percent maps to the scalar"));
}

void a_slider_move_keeps_the_platform_mute_flag()
{
    auto owned = std::make_unique<FakePort>();
    FakePort& fake = *owned;
    fake.reported = {true, true, 0.2};
    voicetyper::platform::MicrophoneLevelController controller(std::move(owned));
    const auto loaded = controller.refresh();
    check_equal(loaded.muted, true, std::string("the muted flag is read from the platform"));

    const auto status = controller.set_percent(80);
    check(status.is_ok(), "setting the level succeeds on a working port");
    check_equal(fake.writes, 1, std::string("the port was asked once"));
    check_equal(fake.last_level, 0.8, std::string("the level that reached the platform"));
    check_equal(fake.last_muted, true,
        std::string("a sensitivity change must not unmute the microphone"));
    check_equal(controller.current().level, 0.8, std::string("the controller remembers the level"));
}

void a_refusing_port_leaves_the_state_alone()
{
    auto owned = std::make_unique<FakePort>();
    FakePort& fake = *owned;
    fake.reported = {true, false, 0.5};
    fake.fail_write = true;
    voicetyper::platform::MicrophoneLevelController controller(std::move(owned));
    static_cast<void>(controller.refresh());

    const auto status = controller.set_percent(90);
    check(!status.is_ok(), "a refused write is a failure");
    check_equal(controller.current().level, 0.5, std::string("the old level is kept"));
}

void a_platform_without_a_level_control_says_so()
{
    voicetyper::platform::MicrophoneLevelController without_port(nullptr);
    check_equal(without_port.available(), false, std::string("no port means no control"));
    const auto status = without_port.set_percent(50);
    check(!status.is_ok(), "writing without a port fails");
    check_equal(static_cast<int>(status.error().code()),
        static_cast<int>(voicetyper::platform::ErrorCode::unavailable), std::string("the error code"));
    check_equal(without_port.current().available, false, std::string("the state stays unavailable"));
}

void an_unavailable_endpoint_is_reported_as_unavailable()
{
    auto owned = std::make_unique<FakePort>();
    FakePort& fake = *owned;
    fake.reported = {};
    voicetyper::platform::MicrophoneLevelController controller(std::move(owned));
    check_equal(controller.refresh().available, false, std::string("the endpoint reports unavailable"));
    check_equal(controller.available(), false, std::string("the controller follows the platform"));
}

} // namespace

int main()
{
    std::printf("microphone-level-contract: the microphone level control\n");
    the_range_mapping_round_trips();
    a_slider_move_keeps_the_platform_mute_flag();
    a_refusing_port_leaves_the_state_alone();
    a_platform_without_a_level_control_says_so();
    an_unavailable_endpoint_is_reported_as_unavailable();
    if (failures == 0) {
        std::printf("microphone-level-contract: OK (%d checks)\n", checks);
        return 0;
    }
    std::printf("microphone-level-contract: %d checks, %d failures\n", checks, failures);
    return 1;
}
