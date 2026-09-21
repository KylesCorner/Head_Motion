/*
 * Unit tests for SyncCommand accel/gyro timestamp pairing.
 *
 * No MMS+ hardware required.
 *
 * Tests:
 *   - exact timestamp match
 *   - timestamps within tolerance
 *   - exact 20 ms tolerance boundary
 *   - 21 ms outside tolerance
 *   - accel older than gyro
 *   - gyro older than accel
 *   - multiple ordered samples
 *   - leftover accel samples
 *   - leftover gyro samples
 */

#include "../src/app/SyncCommand.cpp"

#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void fail(
    const std::string& test_name,
    const std::string& message
) {
    ++failures;

    std::cerr
        << "FAIL: "
        << test_name
        << ": "
        << message
        << '\n';
}

void pass(
    const std::string& test_name
) {
    std::cout
        << "PASS: "
        << test_name
        << '\n';
}

void expectEqual(
    const std::string& test_name,
    std::uint64_t actual,
    std::uint64_t expected,
    const std::string& field
) {
    if (actual != expected) {
        fail(
            test_name,
            field +
            ": expected " +
            std::to_string(expected) +
            ", got " +
            std::to_string(actual)
        );
    }
}

} // namespace


namespace headmotion::app {

constexpr const char* TEST_XSENS_HEADER =
    "PacketCounter,SampleTimeFine,"
    "Euler_X,Euler_Y,Euler_Z,"
    "Acc_X,Acc_Y,Acc_Z,"
    "Gyr_X,Gyr_Y,Gyr_Z,"
    "elapsed_ms,utc_timestamp";


struct PairingTestContext {
    SyncState state;

    std::deque<TimedVectorSample>
        accel_samples;

    std::deque<TimedVectorSample>
        gyro_samples;

    std::filesystem::path path;


    explicit PairingTestContext(
        const std::string& test_name
    ) {
        path =
            std::filesystem::temp_directory_path() /
            (
                "headmotion_pairing_" +
                test_name +
                ".csv"
            );

        state.xsens_csv.open(
            path,
            std::ios::out |
            std::ios::binary |
            std::ios::trunc
        );

        if (!state.xsens_csv.is_open()) {
            throw std::runtime_error(
                "Could not create temporary Xsens CSV"
            );
        }

        state.xsens_csv
            << TEST_XSENS_HEADER
            << '\n';
    }


    ~PairingTestContext()
    {
        if (state.xsens_csv.is_open()) {
            state.xsens_csv.close();
        }

        std::error_code error;

        std::filesystem::remove(
            path,
            error
        );
    }


    void addAccel(
        std::int64_t epoch_ms
    ) {
        accel_samples.push_back(
            TimedVectorSample{
                epoch_ms,
                1.0f,
                2.0f,
                3.0f
            }
        );
    }


    void addGyro(
        std::int64_t epoch_ms
    ) {
        gyro_samples.push_back(
            TimedVectorSample{
                epoch_ms,
                10.0f,
                20.0f,
                30.0f
            }
        );
    }


    bool drain()
    {
        return drainPairQueues(
            state,
            accel_samples,
            gyro_samples
        );
    }
};


void testExactMatch()
{
    constexpr const char* TEST =
        "exact timestamp match";

    PairingTestContext ctx(TEST);

    ctx.addAccel(1000);
    ctx.addGyro(1000);

    if (!ctx.drain()) {
        fail(TEST, "drainPairQueues returned false");
        return;
    }

    expectEqual(
        TEST,
        ctx.state.xsens_rows_written.load(),
        1,
        "rows written"
    );

    expectEqual(
        TEST,
        ctx.state.xsens_unmatched_accel,
        0,
        "unmatched accel"
    );

    expectEqual(
        TEST,
        ctx.state.xsens_unmatched_gyro,
        0,
        "unmatched gyro"
    );

    if (failures == 0) {
        pass(TEST);
    }
}


void testWithinTolerance()
{
    constexpr const char* TEST =
        "timestamps within tolerance";

    const int failures_before =
        failures;

    PairingTestContext ctx(TEST);

    ctx.addAccel(1000);
    ctx.addGyro(1019);

    if (!ctx.drain()) {
        fail(TEST, "drainPairQueues returned false");
        return;
    }

    expectEqual(
        TEST,
        ctx.state.xsens_rows_written.load(),
        1,
        "rows written"
    );

    expectEqual(
        TEST,
        ctx.state.xsens_unmatched_accel,
        0,
        "unmatched accel"
    );

    expectEqual(
        TEST,
        ctx.state.xsens_unmatched_gyro,
        0,
        "unmatched gyro"
    );

    if (failures == failures_before) {
        pass(TEST);
    }
}


void testExactToleranceBoundary()
{
    constexpr const char* TEST =
        "exact 20ms tolerance boundary";

    const int failures_before =
        failures;

    PairingTestContext ctx(TEST);

    ctx.addAccel(1000);
    ctx.addGyro(1020);

    if (!ctx.drain()) {
        fail(TEST, "drainPairQueues returned false");
        return;
    }

    /*
     * Difference == tolerance must still pair.
     */
    expectEqual(
        TEST,
        ctx.state.xsens_rows_written.load(),
        1,
        "rows written"
    );

    expectEqual(
        TEST,
        ctx.state.xsens_unmatched_accel,
        0,
        "unmatched accel"
    );

    expectEqual(
        TEST,
        ctx.state.xsens_unmatched_gyro,
        0,
        "unmatched gyro"
    );

    if (failures == failures_before) {
        pass(TEST);
    }
}


void testOutsideToleranceAccelOlder()
{
    constexpr const char* TEST =
        "21ms difference accel older";

    const int failures_before =
        failures;

    PairingTestContext ctx(TEST);

    ctx.addAccel(1000);
    ctx.addGyro(1021);

    if (!ctx.drain()) {
        fail(TEST, "drainPairQueues returned false");
        return;
    }

    /*
     * accel=1000 is too old to match gyro=1021.
     *
     * drainPairQueues should discard the older accel sample.
     */
    expectEqual(
        TEST,
        ctx.state.xsens_rows_written.load(),
        0,
        "rows written"
    );

    expectEqual(
        TEST,
        ctx.state.xsens_unmatched_accel,
        1,
        "unmatched accel"
    );

    expectEqual(
        TEST,
        ctx.state.xsens_unmatched_gyro,
        0,
        "unmatched gyro"
    );

    expectEqual(
        TEST,
        ctx.accel_samples.size(),
        0,
        "remaining accel queue"
    );

    expectEqual(
        TEST,
        ctx.gyro_samples.size(),
        1,
        "remaining gyro queue"
    );

    if (failures == failures_before) {
        pass(TEST);
    }
}


void testOutsideToleranceGyroOlder()
{
    constexpr const char* TEST =
        "21ms difference gyro older";

    const int failures_before =
        failures;

    PairingTestContext ctx(TEST);

    ctx.addAccel(1021);
    ctx.addGyro(1000);

    if (!ctx.drain()) {
        fail(TEST, "drainPairQueues returned false");
        return;
    }

    expectEqual(
        TEST,
        ctx.state.xsens_rows_written.load(),
        0,
        "rows written"
    );

    expectEqual(
        TEST,
        ctx.state.xsens_unmatched_accel,
        0,
        "unmatched accel"
    );

    expectEqual(
        TEST,
        ctx.state.xsens_unmatched_gyro,
        1,
        "unmatched gyro"
    );

    expectEqual(
        TEST,
        ctx.accel_samples.size(),
        1,
        "remaining accel queue"
    );

    expectEqual(
        TEST,
        ctx.gyro_samples.size(),
        0,
        "remaining gyro queue"
    );

    if (failures == failures_before) {
        pass(TEST);
    }
}


void testMultipleOrderedPairs()
{
    constexpr const char* TEST =
        "multiple ordered pairs";

    const int failures_before =
        failures;

    PairingTestContext ctx(TEST);

    /*
     * All differences are <= 20ms.
     */
    ctx.addAccel(1000);
    ctx.addAccel(1005);
    ctx.addAccel(1010);
    ctx.addAccel(1015);

    ctx.addGyro(1001);
    ctx.addGyro(1006);
    ctx.addGyro(1012);
    ctx.addGyro(1019);

    if (!ctx.drain()) {
        fail(TEST, "drainPairQueues returned false");
        return;
    }

    expectEqual(
        TEST,
        ctx.state.xsens_rows_written.load(),
        4,
        "rows written"
    );

    expectEqual(
        TEST,
        ctx.state.xsens_unmatched_accel,
        0,
        "unmatched accel"
    );

    expectEqual(
        TEST,
        ctx.state.xsens_unmatched_gyro,
        0,
        "unmatched gyro"
    );

    expectEqual(
        TEST,
        ctx.accel_samples.size(),
        0,
        "remaining accel queue"
    );

    expectEqual(
        TEST,
        ctx.gyro_samples.size(),
        0,
        "remaining gyro queue"
    );

    if (failures == failures_before) {
        pass(TEST);
    }
}


void testSkipOldAccelThenPair()
{
    constexpr const char* TEST =
        "skip old accel then pair";

    const int failures_before =
        failures;

    PairingTestContext ctx(TEST);

    /*
     * First accel is too old:
     *
     *   accel 900
     *   accel 1000
     *
     *   gyro 1010
     *
     * Expected:
     *   accel 900  -> unmatched
     *   accel 1000 -> paired with gyro 1010
     */
    ctx.addAccel(900);
    ctx.addAccel(1000);

    ctx.addGyro(1010);

    if (!ctx.drain()) {
        fail(TEST, "drainPairQueues returned false");
        return;
    }

    expectEqual(
        TEST,
        ctx.state.xsens_rows_written.load(),
        1,
        "rows written"
    );

    expectEqual(
        TEST,
        ctx.state.xsens_unmatched_accel,
        1,
        "unmatched accel"
    );

    expectEqual(
        TEST,
        ctx.state.xsens_unmatched_gyro,
        0,
        "unmatched gyro"
    );

    expectEqual(
        TEST,
        ctx.accel_samples.size(),
        0,
        "remaining accel queue"
    );

    expectEqual(
        TEST,
        ctx.gyro_samples.size(),
        0,
        "remaining gyro queue"
    );

    if (failures == failures_before) {
        pass(TEST);
    }
}


void testSkipOldGyroThenPair()
{
    constexpr const char* TEST =
        "skip old gyro then pair";

    const int failures_before =
        failures;

    PairingTestContext ctx(TEST);

    /*
     * First gyro is too old:
     *
     *   accel 1010
     *
     *   gyro 900
     *   gyro 1000
     *
     * Expected:
     *   gyro 900  -> unmatched
     *   gyro 1000 -> paired with accel 1010
     */
    ctx.addAccel(1010);

    ctx.addGyro(900);
    ctx.addGyro(1000);

    if (!ctx.drain()) {
        fail(TEST, "drainPairQueues returned false");
        return;
    }

    expectEqual(
        TEST,
        ctx.state.xsens_rows_written.load(),
        1,
        "rows written"
    );

    expectEqual(
        TEST,
        ctx.state.xsens_unmatched_accel,
        0,
        "unmatched accel"
    );

    expectEqual(
        TEST,
        ctx.state.xsens_unmatched_gyro,
        1,
        "unmatched gyro"
    );

    if (failures == failures_before) {
        pass(TEST);
    }
}


void testRemainingAccel()
{
    constexpr const char* TEST =
        "remaining accel samples";

    const int failures_before =
        failures;

    PairingTestContext ctx(TEST);

    ctx.addAccel(1000);
    ctx.addAccel(1005);
    ctx.addAccel(1010);

    ctx.addGyro(1000);

    if (!ctx.drain()) {
        fail(TEST, "drainPairQueues returned false");
        return;
    }

    /*
     * drainPairQueues cannot declare these final samples unmatched
     * yet because more gyro samples might arrive later.
     */
    expectEqual(
        TEST,
        ctx.state.xsens_rows_written.load(),
        1,
        "rows written"
    );

    expectEqual(
        TEST,
        ctx.accel_samples.size(),
        2,
        "remaining accel queue"
    );

    expectEqual(
        TEST,
        ctx.gyro_samples.size(),
        0,
        "remaining gyro queue"
    );

    if (failures == failures_before) {
        pass(TEST);
    }
}


void testRemainingGyro()
{
    constexpr const char* TEST =
        "remaining gyro samples";

    const int failures_before =
        failures;

    PairingTestContext ctx(TEST);

    ctx.addAccel(1000);

    ctx.addGyro(1000);
    ctx.addGyro(1005);
    ctx.addGyro(1010);

    if (!ctx.drain()) {
        fail(TEST, "drainPairQueues returned false");
        return;
    }

    expectEqual(
        TEST,
        ctx.state.xsens_rows_written.load(),
        1,
        "rows written"
    );

    expectEqual(
        TEST,
        ctx.accel_samples.size(),
        0,
        "remaining accel queue"
    );

    expectEqual(
        TEST,
        ctx.gyro_samples.size(),
        2,
        "remaining gyro queue"
    );

    if (failures == failures_before) {
        pass(TEST);
    }
}


void testCustomTolerance()
{
    constexpr const char* TEST =
        "custom pairing tolerance";

    const int failures_before =
        failures;

    PairingTestContext ctx(TEST);

    ctx.state.xsens_pair_tolerance_ms =
        5;

    /*
     * Difference is 6ms.
     *
     * This would pair under the normal 20ms tolerance,
     * but must not pair with a 5ms tolerance.
     */
    ctx.addAccel(1000);
    ctx.addGyro(1006);

    if (!ctx.drain()) {
        fail(TEST, "drainPairQueues returned false");
        return;
    }

    expectEqual(
        TEST,
        ctx.state.xsens_rows_written.load(),
        0,
        "rows written"
    );

    expectEqual(
        TEST,
        ctx.state.xsens_unmatched_accel,
        1,
        "unmatched accel"
    );

    if (failures == failures_before) {
        pass(TEST);
    }
}


int runPairingTests()
{
    testExactMatch();
    testWithinTolerance();
    testExactToleranceBoundary();

    testOutsideToleranceAccelOlder();
    testOutsideToleranceGyroOlder();

    testMultipleOrderedPairs();

    testSkipOldAccelThenPair();
    testSkipOldGyroThenPair();

    testRemainingAccel();
    testRemainingGyro();

    testCustomTolerance();

    return failures;
}

} // namespace headmotion::app


int main()
{
    std::cout
        << "========================================\n"
        << "HeadMotion Sync Pairing Unit Tests\n"
        << "========================================\n\n";

    const int result =
        headmotion::app::runPairingTests();

    if (result != 0) {
        std::cerr
            << "\n"
            << result
            << " pairing test(s) FAILED\n";

        return 1;
    }

    std::cout
        << "\nAll sync pairing tests PASSED\n";

    return 0;
}