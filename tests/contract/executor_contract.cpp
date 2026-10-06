#include "platform/api/executor.hpp"

#include <cstdio>

namespace {

int failures = 0;

void check(bool condition, const char* what)
{
    if (!condition) {
        ++failures;
        std::printf("FAIL %s\n", what);
    }
}

} // namespace

int main()
{
    using namespace voicetyper;

    platform::ManualExecutor manual;
    int ran = 0;
    manual.post([&ran] { ++ran; });
    check(manual.pending() == 1, "manual executor queues a task");
    check(manual.drain().is_ok(), "manual drain succeeds");
    check(ran == 1, "manual drain runs the task");
    check(manual.pending() == 0, "manual queue is empty after drain");

    const auto stale_generation = manual.generation();
    manual.advance_generation();
    manual.post(stale_generation, [&ran] { ++ran; });
    check(manual.drain().is_ok(), "stale task drain succeeds");
    check(ran == 1, "stale epoch task is dropped");

    const auto empty = manual.invoke({}, std::chrono::milliseconds{1});
    check(empty.code() == domain::ErrorCode::invalid_argument, "empty task is rejected");

    const auto threw = manual.invoke([] { throw 1; }, std::chrono::milliseconds{1});
    check(threw.code() == domain::ErrorCode::internal, "throwing task becomes internal error");

    platform::InlineExecutor inline_executor;
    int inline_ran = 0;
    inline_executor.post([&inline_ran] { ++inline_ran; });
    check(inline_ran == 1, "inline executor runs immediately");
    check(inline_executor.shutdown(std::chrono::milliseconds{1}).is_ok(), "inline shutdown succeeds");

    if (failures != 0) {
        std::printf("executor-contract: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("executor-contract: OK\n");
    return 0;
}
