/*
 * Sync queue overflow unit test.
 *
 * No MMS+ hardware required.
 *
 * Verifies that when SyncCommand's bounded queue reaches
 * MAX_QUEUED_SAMPLES:
 *
 *   - the next enqueue fails
 *   - queue_overflow becomes true
 *   - writer_failed becomes true
 *   - an explanatory writer error is recorded
 *   - the queue never grows beyond the configured limit
 *   - subsequent enqueues fail cleanly
 *
 * This directly exercises the private SyncCommand queue implementation
 * by including SyncCommand.cpp.
 */

#include "../src/app/SyncCommand.cpp"

#include <cstdint>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void fail(
    const std::string& message
) {
    ++failures;

    std::cerr
        << "FAIL: "
        << message
        << '\n';
}

void pass(
    const std::string& message
) {
    std::cout
        << "PASS: "
        << message
        << '\n';
}

} // namespace


namespace headmotion::app {

int runQueueOverflowTest()
{
    std::cout
        << "========================================\n"
        << "HeadMotion Sync Queue Overflow Test\n"
        << "========================================\n\n";

    std::cout
        << "Queue capacity: "
        << MAX_QUEUED_SAMPLES
        << " samples\n\n";

    SyncState state;

    /*
     * Deliberately DO NOT start csvWriterMain().
     *
     * That guarantees nothing drains sample_queue while we fill it,
     * allowing us to deterministically reach the queue's hard limit.
     */
    QueuedSample sample;

    sample.kind =
        SampleKind::Acceleration;

    sample.epoch_ms =
        1760000000000LL;

    sample.x = 1.0f;
    sample.y = 2.0f;
    sample.z = 3.0f;

    /*
     * ---------------------------------------------------------
     * Fill the queue exactly to capacity.
     * ---------------------------------------------------------
     */

    for (
        std::size_t i = 0;
        i < MAX_QUEUED_SAMPLES;
        ++i
    ) {
        sample.epoch_ms =
            1760000000000LL +
            static_cast<std::int64_t>(i);

        const bool accepted =
            enqueueSample(
                state,
                sample
            );

        if (!accepted) {
            fail(
                "enqueue failed before queue reached capacity "
                "at index " +
                std::to_string(i)
            );

            return failures;
        }
    }

    {
        std::lock_guard<std::mutex> lock(
            state.queue_mutex
        );

        if (
            state.sample_queue.size() !=
            MAX_QUEUED_SAMPLES
        ) {
            fail(
                "queue size after filling was " +
                std::to_string(
                    state.sample_queue.size()
                ) +
                ", expected " +
                std::to_string(
                    MAX_QUEUED_SAMPLES
                )
            );
        }
        else {
            pass(
                "queue reached configured capacity"
            );
        }
    }

    if (state.queue_overflow.load()) {
        fail(
            "queue_overflow became true before exceeding capacity"
        );
    }
    else {
        pass(
            "queue_overflow remains false at exact capacity"
        );
    }

    if (state.writer_failed.load()) {
        fail(
            "writer_failed became true before exceeding capacity"
        );
    }
    else {
        pass(
            "writer_failed remains false at exact capacity"
        );
    }

    /*
     * ---------------------------------------------------------
     * One additional sample must trigger overflow.
     * ---------------------------------------------------------
     */

    sample.epoch_ms += 1;

    const bool overflow_enqueue_result =
        enqueueSample(
            state,
            sample
        );

    if (overflow_enqueue_result) {
        fail(
            "enqueue unexpectedly succeeded after queue reached capacity"
        );
    }
    else {
        pass(
            "enqueue rejected sample beyond queue capacity"
        );
    }

    if (!state.queue_overflow.load()) {
        fail(
            "queue_overflow was not set after overflow"
        );
    }
    else {
        pass(
            "queue_overflow set after overflow"
        );
    }

    if (!state.writer_failed.load()) {
        fail(
            "writer_failed was not set after queue overflow"
        );
    }
    else {
        pass(
            "writer_failed set after queue overflow"
        );
    }

    /*
     * The overflow sample itself must NOT be added.
     */
    {
        std::lock_guard<std::mutex> lock(
            state.queue_mutex
        );

        if (
            state.sample_queue.size() >
            MAX_QUEUED_SAMPLES
        ) {
            fail(
                "queue grew beyond MAX_QUEUED_SAMPLES"
            );
        }
        else if (
            state.sample_queue.size() !=
            MAX_QUEUED_SAMPLES
        ) {
            fail(
                "unexpected queue size after overflow: " +
                std::to_string(
                    state.sample_queue.size()
                )
            );
        }
        else {
            pass(
                "queue never exceeded configured capacity"
            );
        }
    }

    /*
     * ---------------------------------------------------------
     * Verify useful error reporting.
     * ---------------------------------------------------------
     */

    const std::string error =
        writerError(state);

    if (error.empty()) {
        fail(
            "writer error message is empty"
        );
    }
    else {
        pass(
            "writer error message was recorded"
        );

        std::cout
            << "      error: "
            << error
            << '\n';
    }

    if (
        error.find("overflow") ==
        std::string::npos
    ) {
        fail(
            "writer error does not mention overflow"
        );
    }
    else {
        pass(
            "writer error identifies queue overflow"
        );
    }

    /*
     * ---------------------------------------------------------
     * Subsequent callbacks must fail safely.
     * ---------------------------------------------------------
     */

    for (
        std::size_t i = 0;
        i < 100;
        ++i
    ) {
        if (
            enqueueSample(
                state,
                sample
            )
        ) {
            fail(
                "enqueue succeeded after writer entered failed state"
            );

            break;
        }
    }

    if (failures == 0) {
        pass(
            "subsequent enqueues fail cleanly"
        );
    }

    /*
     * Queue contents must still be unchanged.
     */
    {
        std::lock_guard<std::mutex> lock(
            state.queue_mutex
        );

        if (
            state.sample_queue.size() !=
            MAX_QUEUED_SAMPLES
        ) {
            fail(
                "failed enqueues modified queue contents"
            );
        }
        else {
            pass(
                "failed enqueues do not modify queue"
            );
        }
    }

    /*
     * ---------------------------------------------------------
     * Final result
     * ---------------------------------------------------------
     */

    std::cout << '\n';

    if (failures != 0) {
        std::cerr
            << failures
            << " queue overflow assertion(s) FAILED\n";

        return 1;
    }

    std::cout
        << "All queue overflow checks PASSED\n";

    return 0;
}

} // namespace headmotion::app


int main()
{
    return
        headmotion::app::
            runQueueOverflowTest();
}