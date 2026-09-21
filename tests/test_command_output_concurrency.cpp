/*
 * CommandOutput concurrency stress test.
 *
 * No hardware required.
 *
 * Verifies:
 *   - many threads can share one CommandOutput
 *   - its sink is never entered concurrently
 *   - no events are lost
 *   - no events are duplicated
 *   - every JSONL event remains exactly one physical line
 *   - JSON escaping remains intact
 *   - per-thread event ordering is preserved
 *   - jsonFileSink output matches the in-memory sink exactly
 *
 * Optional environment variables:
 *
 *   HEADMOTION_COMMAND_OUTPUT_THREADS
 *   HEADMOTION_COMMAND_OUTPUT_EVENTS
 *
 * Defaults:
 *
 *   16 threads
 *   2000 events/thread
 *
 * = 32,000 events total
 */

#include "headmotion/app/CommandOutput.hpp"

#include <atomic>
#include <barrier>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using headmotion::app::CommandEvent;
using headmotion::app::CommandOutput;

constexpr std::size_t DEFAULT_THREAD_COUNT = 16;
constexpr std::size_t DEFAULT_EVENTS_PER_THREAD = 2000;

constexpr const char* COMMAND_NAME =
    "command-output-concurrency";

/*
 * Includes characters that must be JSON escaped.
 */
constexpr const char* STRESS_MESSAGE =
    "quote=\" slash=\\ newline=\n tab=\t";


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


std::size_t readSizeEnv(
    const char* name,
    std::size_t default_value
) {
    const char* value =
        std::getenv(name);

    if (
        value == nullptr ||
        *value == '\0'
    ) {
        return default_value;
    }

    try {
        const unsigned long long parsed =
            std::stoull(value);

        if (
            parsed == 0 ||
            parsed >
                static_cast<unsigned long long>(
                    std::numeric_limits<std::size_t>::max()
                )
        ) {
            throw std::runtime_error(
                "out of range"
            );
        }

        return static_cast<std::size_t>(
            parsed
        );
    }
    catch (...) {
        std::cerr
            << "Invalid value for "
            << name
            << ": "
            << value
            << '\n';

        std::exit(2);
    }
}


std::filesystem::path makeOutputPath()
{
    const auto timestamp =
        std::chrono::steady_clock::now()
            .time_since_epoch()
            .count();

    return
        std::filesystem::temp_directory_path() /
        (
            "headmotion_command_output_concurrency_" +
            std::to_string(timestamp) +
            ".jsonl"
        );
}


/*
 * Build the exact JSON line that CommandOutput should produce.
 *
 * This is intentionally built independently instead of calling
 * CommandOutput::toJsonLine(), because we want the test to detect
 * malformed serialization too.
 */
std::string expectedLine(
    std::size_t thread_id,
    std::size_t sequence
) {
    return
        "{\"event\":\"stress\","
        "\"command\":\"command-output-concurrency\","
        "\"thread_id\":" +
        std::to_string(thread_id) +
        ",\"sequence\":" +
        std::to_string(sequence) +
        ",\"message\":\""
        "quote=\\\" "
        "slash=\\\\ "
        "newline=\\n "
        "tab=\\t"
        "\"}";
}


bool parseUnsignedField(
    const std::string& line,
    const std::string& marker,
    std::uint64_t& value
) {
    const std::size_t begin =
        line.find(marker);

    if (begin == std::string::npos) {
        return false;
    }

    const std::size_t value_begin =
        begin +
        marker.size();

    std::size_t value_end =
        value_begin;

    while (
        value_end < line.size() &&
        line[value_end] >= '0' &&
        line[value_end] <= '9'
    ) {
        ++value_end;
    }

    if (value_end == value_begin) {
        return false;
    }

    const char* first =
        line.data() +
        static_cast<std::ptrdiff_t>(
            value_begin
        );

    const char* last =
        line.data() +
        static_cast<std::ptrdiff_t>(
            value_end
        );

    const auto result =
        std::from_chars(
            first,
            last,
            value
        );

    return
        result.ec ==
            std::errc{} &&
        result.ptr ==
            last;
}


bool containsRawControlCharacter(
    const std::string& line
) {
    for (const unsigned char ch : line) {
        if (ch < 0x20) {
            return true;
        }
    }

    return false;
}

} // namespace


int main()
{
    const std::size_t thread_count =
        readSizeEnv(
            "HEADMOTION_COMMAND_OUTPUT_THREADS",
            DEFAULT_THREAD_COUNT
        );

    const std::size_t events_per_thread =
        readSizeEnv(
            "HEADMOTION_COMMAND_OUTPUT_EVENTS",
            DEFAULT_EVENTS_PER_THREAD
        );

    const std::size_t total_events =
        thread_count *
        events_per_thread;

    const std::filesystem::path output_path =
        makeOutputPath();


    std::cout
        << "========================================\n"
        << "HeadMotion CommandOutput Concurrency Test\n"
        << "========================================\n\n"
        << "Threads:          "
        << thread_count
        << '\n'
        << "Events/thread:    "
        << events_per_thread
        << '\n'
        << "Total events:     "
        << total_events
        << '\n'
        << "Output:           "
        << output_path
        << "\n\n";


    /*
     * ============================================================
     * Concurrency probe
     * ============================================================
     *
     * active_sink_calls tells us whether CommandOutput invokes its
     * sink concurrently.
     *
     * For one CommandOutput instance, this must NEVER exceed 1.
     */

    std::atomic<std::uint32_t>
        active_sink_calls{0};

    std::atomic<std::uint64_t>
        overlapping_sink_calls{0};

    std::atomic<std::uint32_t>
        max_active_sink_calls{0};


    /*
     * The vector itself is protected independently.
     *
     * We do NOT want a broken CommandOutput implementation to cause
     * undefined behavior inside the test.
     */
    std::mutex captured_mutex;

    std::vector<std::string>
        captured_lines;

    captured_lines.reserve(
        total_events
    );


    CommandOutput::Sink probe_sink =
        [
            &active_sink_calls,
            &overlapping_sink_calls,
            &max_active_sink_calls,
            &captured_mutex,
            &captured_lines
        ](
            const CommandEvent& event
        ) {
            const std::uint32_t active_now =
                active_sink_calls.fetch_add(
                    1,
                    std::memory_order_acq_rel
                ) +
                1;

            /*
             * Track maximum observed concurrency.
             */
            std::uint32_t previous_max =
                max_active_sink_calls.load(
                    std::memory_order_relaxed
                );

            while (
                active_now >
                    previous_max &&
                !max_active_sink_calls
                    .compare_exchange_weak(
                        previous_max,
                        active_now,
                        std::memory_order_relaxed
                    )
            ) {
            }


            if (active_now > 1) {
                overlapping_sink_calls.fetch_add(
                    1,
                    std::memory_order_relaxed
                );
            }


            /*
             * Deliberately widen the concurrency window.
             *
             * If CommandOutput loses its emit mutex in the future,
             * this makes overlapping sink calls very easy to detect.
             */
            std::this_thread::sleep_for(
                std::chrono::microseconds(10)
            );


            const std::string line =
                CommandOutput::toJsonLine(
                    event
                );


            {
                std::lock_guard<std::mutex> lock(
                    captured_mutex
                );

                captured_lines.push_back(
                    line
                );
            }


            active_sink_calls.fetch_sub(
                1,
                std::memory_order_acq_rel
            );
        };


    /*
     * ============================================================
     * Emit events
     * ============================================================
     *
     * Both sinks receive the same event:
     *
     *   probe sink
     *       +
     *   real jsonFileSink
     */

    {
        CommandOutput output(
            COMMAND_NAME,
            CommandOutput::combineSinks(
                {
                    probe_sink,
                    CommandOutput::jsonFileSink(
                        output_path
                    )
                }
            )
        );


        std::barrier<> start_barrier(
            static_cast<std::ptrdiff_t>(
                thread_count
            )
        );


        std::vector<std::thread>
            workers;

        workers.reserve(
            thread_count
        );


        for (
            std::size_t thread_id = 0;
            thread_id < thread_count;
            ++thread_id
        ) {
            workers.emplace_back(
                [
                    &output,
                    &start_barrier,
                    thread_id,
                    events_per_thread
                ] {
                    /*
                     * Make all producers hit CommandOutput at roughly
                     * the same time.
                     */
                    start_barrier.arrive_and_wait();


                    for (
                        std::size_t sequence = 0;
                        sequence <
                            events_per_thread;
                        ++sequence
                    ) {
                        output.event(
                            "stress",
                            {
                                {
                                    "thread_id",
                                    static_cast<std::uint64_t>(
                                        thread_id
                                    )
                                },
                                {
                                    "sequence",
                                    static_cast<std::uint64_t>(
                                        sequence
                                    )
                                },
                                {
                                    "message",
                                    STRESS_MESSAGE
                                }
                            }
                        );


                        /*
                         * Periodically encourage scheduler
                         * interleaving between producer threads.
                         */
                        if (
                            (sequence % 128) ==
                            0
                        ) {
                            std::this_thread::yield();
                        }
                    }
                }
            );
        }


        for (auto& worker : workers) {
            worker.join();
        }

        /*
         * output is destroyed here, closing jsonFileSink's owned
         * ofstream before we inspect the file.
         */
    }


    /*
     * ============================================================
     * Verify sink serialization
     * ============================================================
     */

    if (
        overlapping_sink_calls.load() !=
        0
    ) {
        fail(
            "CommandOutput invoked its sink concurrently " +
            std::to_string(
                overlapping_sink_calls.load()
            ) +
            " time(s)"
        );
    }
    else {
        pass(
            "sink was never entered concurrently"
        );
    }


    if (
        max_active_sink_calls.load() !=
        1
    ) {
        fail(
            "maximum simultaneous sink calls was " +
            std::to_string(
                max_active_sink_calls.load()
            ) +
            ", expected 1"
        );
    }
    else {
        pass(
            "maximum sink concurrency was exactly 1"
        );
    }


    /*
     * ============================================================
     * Verify in-memory event count
     * ============================================================
     */

    if (
        captured_lines.size() !=
        total_events
    ) {
        fail(
            "in-memory sink captured " +
            std::to_string(
                captured_lines.size()
            ) +
            " events, expected " +
            std::to_string(
                total_events
            )
        );
    }
    else {
        pass(
            "no events missing from in-memory sink"
        );
    }


    /*
     * ============================================================
     * Verify actual JSONL file
     * ============================================================
     */

    std::ifstream file(
        output_path
    );

    if (!file.is_open()) {
        fail(
            "could not open JSONL output file"
        );
    }


    /*
     * seen[event_index]
     *
     * event_index =
     *     thread_id * events_per_thread +
     *     sequence
     *
     * Lets us detect both missing and duplicate events.
     */
    std::vector<std::uint8_t> seen(
        total_events,
        0
    );


    /*
     * Track the next expected sequence independently for every
     * producer thread.
     *
     * Cross-thread ordering is intentionally unspecified.
     *
     * Within each producer thread, however:
     *
     *     0, 1, 2, 3, ...
     *
     * must remain ordered.
     */
    std::vector<std::uint64_t>
        next_sequence(
            thread_count,
            0
        );


    std::size_t line_index = 0;
    std::string line;


    while (
        file.is_open() &&
        std::getline(file, line)
    ) {
        /*
         * --------------------------------------------------------
         * One JSON object per physical line
         * --------------------------------------------------------
         */

        if (
            line.empty() ||
            line.front() != '{' ||
            line.back() != '}'
        ) {
            fail(
                "line " +
                std::to_string(line_index) +
                " is not one complete JSON object"
            );
        }


        /*
         * JSON strings may contain escaped \n and \t, but they must
         * never appear as raw control characters inside a JSONL
         * record.
         */
        if (
            containsRawControlCharacter(
                line
            )
        ) {
            fail(
                "line " +
                std::to_string(line_index) +
                " contains a raw control character"
            );
        }


        /*
         * --------------------------------------------------------
         * Parse identifying fields
         * --------------------------------------------------------
         */

        std::uint64_t thread_id = 0;
        std::uint64_t sequence = 0;


        const bool thread_ok =
            parseUnsignedField(
                line,
                "\"thread_id\":",
                thread_id
            );

        const bool sequence_ok =
            parseUnsignedField(
                line,
                "\"sequence\":",
                sequence
            );


        if (
            !thread_ok ||
            !sequence_ok
        ) {
            fail(
                "line " +
                std::to_string(line_index) +
                " is missing thread_id or sequence"
            );

            ++line_index;
            continue;
        }


        if (
            thread_id >=
            thread_count
        ) {
            fail(
                "line " +
                std::to_string(line_index) +
                " contains invalid thread_id " +
                std::to_string(thread_id)
            );

            ++line_index;
            continue;
        }


        if (
            sequence >=
            events_per_thread
        ) {
            fail(
                "line " +
                std::to_string(line_index) +
                " contains invalid sequence " +
                std::to_string(sequence)
            );

            ++line_index;
            continue;
        }


        /*
         * --------------------------------------------------------
         * Verify complete JSON serialization
         * --------------------------------------------------------
         */

        const std::string expected =
            expectedLine(
                static_cast<std::size_t>(
                    thread_id
                ),
                static_cast<std::size_t>(
                    sequence
                )
            );


        if (line != expected) {
            fail(
                "JSON mismatch at line " +
                std::to_string(line_index)
            );

            std::cerr
                << "      expected: "
                << expected
                << '\n'
                << "      actual:   "
                << line
                << '\n';
        }


        /*
         * --------------------------------------------------------
         * Verify no duplicate events
         * --------------------------------------------------------
         */

        const std::size_t event_index =
            static_cast<std::size_t>(
                thread_id
            ) *
            events_per_thread +
            static_cast<std::size_t>(
                sequence
            );


        if (seen[event_index] != 0) {
            fail(
                "duplicate event: thread " +
                std::to_string(thread_id) +
                ", sequence " +
                std::to_string(sequence)
            );
        }
        else {
            seen[event_index] = 1;
        }


        /*
         * --------------------------------------------------------
         * Verify per-thread ordering
         * --------------------------------------------------------
         */

        if (
            sequence !=
            next_sequence[
                static_cast<std::size_t>(
                    thread_id
                )
            ]
        ) {
            fail(
                "thread " +
                std::to_string(thread_id) +
                " ordering violation: expected sequence " +
                std::to_string(
                    next_sequence[
                        static_cast<std::size_t>(
                            thread_id
                        )
                    ]
                ) +
                ", saw " +
                std::to_string(sequence)
            );
        }
        else {
            ++next_sequence[
                static_cast<std::size_t>(
                    thread_id
                )
            ];
        }


        /*
         * --------------------------------------------------------
         * Verify file sink and in-memory sink saw identical order
         * --------------------------------------------------------
         */

        if (
            line_index <
            captured_lines.size()
        ) {
            if (
                line !=
                captured_lines[line_index]
            ) {
                fail(
                    "file sink ordering differs from "
                    "in-memory sink at line " +
                    std::to_string(
                        line_index
                    )
                );
            }
        }


        ++line_index;
    }


    /*
     * ============================================================
     * Final accounting
     * ============================================================
     */

    if (
        line_index !=
        total_events
    ) {
        fail(
            "JSONL file contains " +
            std::to_string(line_index) +
            " lines, expected " +
            std::to_string(total_events)
        );
    }
    else {
        pass(
            "JSONL file contains exactly one line per event"
        );
    }


    std::size_t missing_events = 0;

    for (
        const std::uint8_t value :
        seen
    ) {
        if (value == 0) {
            ++missing_events;
        }
    }


    if (missing_events != 0) {
        fail(
            std::to_string(missing_events) +
            " expected event(s) are missing"
        );
    }
    else {
        pass(
            "no events were lost or duplicated"
        );
    }


    bool ordering_ok = true;

    for (
        std::size_t thread_id = 0;
        thread_id < thread_count;
        ++thread_id
    ) {
        if (
            next_sequence[thread_id] !=
            events_per_thread
        ) {
            ordering_ok = false;

            break;
        }
    }


    if (ordering_ok) {
        pass(
            "per-thread event ordering was preserved"
        );
    }
    else {
        fail(
            "one or more producer threads lost ordering"
        );
    }


    /*
     * Because the two sinks receive each event sequentially through
     * combineSinks(), their complete output ordering should match.
     */
    if (
        captured_lines.size() ==
            line_index
    ) {
        pass(
            "in-memory and JSONL sinks received identical event counts"
        );
    }


    /*
     * ============================================================
     * Cleanup / result
     * ============================================================
     */

    if (failures != 0) {
        std::cerr
            << "\n"
            << failures
            << " CommandOutput concurrency assertion(s) FAILED\n"
            << "JSONL artifact preserved at:\n  "
            << output_path
            << '\n';

        return 1;
    }


    std::error_code error;

    std::filesystem::remove(
        output_path,
        error
    );


    std::cout
        << "\n"
        << "PASS: JSON serialization remained valid under concurrency\n"
        << "PASS: JSON escaping remained intact\n"
        << "PASS: all "
        << total_events
        << " events survived concurrent emission\n\n"
        << "COMMAND OUTPUT CONCURRENCY TEST PASSED\n";


    return 0;
}