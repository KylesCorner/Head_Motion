/*
 * Sync CSV filename collision tests.
 *
 * No MMS+ hardware required.
 *
 * Tests:
 *   - unused device ID gets unsuffixed filenames
 *   - existing imu_N.csv forces the next suffix
 *   - existing battery_N.csv also reserves that suffix
 *   - existing legacy imu_N.csv also reserves that suffix
 *   - several occupied suffixes select the next free suffix
 *   - all three output files always use the same suffix
 *   - device IDs are sanitized before becoming filenames
 *   - sequential sessions advance suffixes after files are created
 *   - many virtual devices may select paths concurrently without
 *     colliding with one another
 */

#include "../src/app/SyncCommand.cpp"

#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

int failures = 0;


void fail(
    const std::string& test,
    const std::string& message
) {
    ++failures;

    std::cerr
        << "FAIL: "
        << test
        << ": "
        << message
        << '\n';
}


void pass(
    const std::string& test
) {
    std::cout
        << "PASS: "
        << test
        << '\n';
}


void touch(
    const std::filesystem::path& path
) {
    std::ofstream file(
        path,
        std::ios::out |
        std::ios::trunc
    );

    if (!file.is_open()) {
        throw std::runtime_error(
            "Could not create test file: " +
            path.string()
        );
    }
}


std::filesystem::path makeTempDirectory()
{
    const auto stamp =
        std::chrono::steady_clock::now()
            .time_since_epoch()
            .count();

    const auto path =
        std::filesystem::temp_directory_path() /
        (
            "headmotion_filename_collision_" +
            std::to_string(stamp)
        );

    std::filesystem::create_directories(
        path
    );

    return path;
}


bool pathEquals(
    const std::filesystem::path& actual,
    const std::filesystem::path& expected
) {
    return
        actual.lexically_normal() ==
        expected.lexically_normal();
}

} // namespace


namespace headmotion::app {

/*
 * ---------------------------------------------------------
 * Completely unused device ID
 * ---------------------------------------------------------
 */

void testUnusedDevice(
    const std::filesystem::path& root
) {
    constexpr const char* TEST =
        "unused device uses base filenames";

    const int failures_before =
        failures;

    const auto dir =
        root / "unused";

    std::filesystem::create_directories(
        dir
    );

    const CsvOutputPaths paths =
        chooseUnusedCsvOutputPaths(
            dir,
            "0561E1"
        );

    if (
        !pathEquals(
            paths.xsens,
            dir / "imu_0561E1.csv"
        )
    ) {
        fail(
            TEST,
            "unexpected Xsens path: " +
            paths.xsens.string()
        );
    }

    if (
        !pathEquals(
            paths.imu,
            dir / "imu_legacy_0561E1.csv"
        )
    ) {
        fail(
            TEST,
            "unexpected legacy IMU path: " +
            paths.imu.string()
        );
    }

    if (
        !pathEquals(
            paths.battery,
            dir / "battery_0561E1.csv"
        )
    ) {
        fail(
            TEST,
            "unexpected battery path: " +
            paths.battery.string()
        );
    }

    if (failures == failures_before) {
        pass(TEST);
    }
}


/*
 * ---------------------------------------------------------
 * Basic collision chain
 * ---------------------------------------------------------
 */

void testThreeExistingCollisions(
    const std::filesystem::path& root
) {
    constexpr const char* TEST =
        "three collisions select suffix 3";

    const int failures_before =
        failures;

    const auto dir =
        root / "three_collisions";

    std::filesystem::create_directories(
        dir
    );

    /*
     * It is sufficient for ONE member of each session suffix to
     * exist. The whole suffix must then be considered occupied.
     */
    touch(
        dir /
        "imu_0561E1.csv"
    );

    touch(
        dir /
        "imu_0561E1_1.csv"
    );

    touch(
        dir /
        "imu_0561E1_2.csv"
    );


    const CsvOutputPaths paths =
        chooseUnusedCsvOutputPaths(
            dir,
            "0561E1"
        );


    const auto expected_xsens =
        dir /
        "imu_0561E1_3.csv";

    const auto expected_imu =
        dir /
        "imu_legacy_0561E1_3.csv";

    const auto expected_battery =
        dir /
        "battery_0561E1_3.csv";


    if (
        !pathEquals(
            paths.xsens,
            expected_xsens
        )
    ) {
        fail(
            TEST,
            "expected " +
            expected_xsens.string() +
            ", got " +
            paths.xsens.string()
        );
    }


    if (
        !pathEquals(
            paths.imu,
            expected_imu
        )
    ) {
        fail(
            TEST,
            "legacy IMU did not share suffix 3"
        );
    }


    if (
        !pathEquals(
            paths.battery,
            expected_battery
        )
    ) {
        fail(
            TEST,
            "battery CSV did not share suffix 3"
        );
    }


    if (failures == failures_before) {
        pass(TEST);
    }
}


/*
 * ---------------------------------------------------------
 * Battery collision reserves the entire suffix
 * ---------------------------------------------------------
 */

void testBatteryCollision(
    const std::filesystem::path& root
) {
    constexpr const char* TEST =
        "battery collision reserves suffix";

    const int failures_before =
        failures;

    const auto dir =
        root / "battery_collision";

    std::filesystem::create_directories(
        dir
    );

    touch(
        dir /
        "battery_0561E1.csv"
    );


    const CsvOutputPaths paths =
        chooseUnusedCsvOutputPaths(
            dir,
            "0561E1"
        );


    if (
        paths.xsens.filename() !=
        "imu_0561E1_1.csv"
    ) {
        fail(
            TEST,
            "Xsens file did not advance to suffix 1"
        );
    }


    if (
        paths.imu.filename() !=
        "imu_legacy_0561E1_1.csv"
    ) {
        fail(
            TEST,
            "legacy file did not advance to suffix 1"
        );
    }


    if (
        paths.battery.filename() !=
        "battery_0561E1_1.csv"
    ) {
        fail(
            TEST,
            "battery file did not advance to suffix 1"
        );
    }


    if (failures == failures_before) {
        pass(TEST);
    }
}


/*
 * ---------------------------------------------------------
 * Legacy IMU collision reserves the entire suffix
 * ---------------------------------------------------------
 */

void testLegacyCollision(
    const std::filesystem::path& root
) {
    constexpr const char* TEST =
        "legacy IMU collision reserves suffix";

    const int failures_before =
        failures;

    const auto dir =
        root / "legacy_collision";

    std::filesystem::create_directories(
        dir
    );

    touch(
        dir /
        "imu_legacy_0561E1.csv"
    );


    const CsvOutputPaths paths =
        chooseUnusedCsvOutputPaths(
            dir,
            "0561E1"
        );


    if (
        paths.xsens.filename() !=
        "imu_0561E1_1.csv"
    ) {
        fail(
            TEST,
            "did not advance past legacy collision"
        );
    }


    if (failures == failures_before) {
        pass(TEST);
    }
}


/*
 * ---------------------------------------------------------
 * Device ID sanitization
 * ---------------------------------------------------------
 */

void testDeviceIdSanitization(
    const std::filesystem::path& root
) {
    constexpr const char* TEST =
        "device ID is sanitized";

    const int failures_before =
        failures;

    const auto dir =
        root / "sanitization";

    std::filesystem::create_directories(
        dir
    );


    const CsvOutputPaths paths =
        chooseUnusedCsvOutputPaths(
            dir,
            "  MMS:0561E1/?  "
        );


    /*
     * sanitizeFilenameToken():
     *
     * spaces/punctuation -> '_'
     * leading/trailing '_' removed
     *
     * Interior runs of '_' are intentionally preserved.
     */
    const std::string filename =
        paths.xsens.filename().string();


    if (
        filename.find('/') !=
            std::string::npos ||
        filename.find(':') !=
            std::string::npos ||
        filename.find('?') !=
            std::string::npos ||
        filename.find(' ') !=
            std::string::npos
    ) {
        fail(
            TEST,
            "unsafe filename character remained: " +
            filename
        );
    }


    if (
        filename.rfind(
            "imu_",
            0
        ) != 0
    ) {
        fail(
            TEST,
            "unexpected filename prefix: " +
            filename
        );
    }


    if (
        paths.xsens.parent_path() !=
        dir
    ) {
        fail(
            TEST,
            "sanitized ID escaped output directory"
        );
    }


    if (failures == failures_before) {
        pass(TEST);
    }
}


/*
 * ---------------------------------------------------------
 * Sequential sessions
 * ---------------------------------------------------------
 */

void testSequentialSessions(
    const std::filesystem::path& root
) {
    constexpr const char* TEST =
        "sequential sessions advance suffix";

    const int failures_before =
        failures;

    const auto dir =
        root / "sequential";

    std::filesystem::create_directories(
        dir
    );


    /*
     * Session 0
     */
    const CsvOutputPaths first =
        chooseUnusedCsvOutputPaths(
            dir,
            "0561E1"
        );


    if (
        first.xsens.filename() !=
        "imu_0561E1.csv"
    ) {
        fail(
            TEST,
            "first session did not use base filename"
        );
    }


    /*
     * Simulate runSyncCommand opening/creating all of the selected
     * output files.
     */
    touch(first.xsens);
    touch(first.imu);
    touch(first.battery);


    /*
     * Session 1
     */
    const CsvOutputPaths second =
        chooseUnusedCsvOutputPaths(
            dir,
            "0561E1"
        );


    if (
        second.xsens.filename() !=
        "imu_0561E1_1.csv"
    ) {
        fail(
            TEST,
            "second session did not use suffix 1"
        );
    }


    touch(second.xsens);
    touch(second.imu);
    touch(second.battery);


    /*
     * Session 2
     */
    const CsvOutputPaths third =
        chooseUnusedCsvOutputPaths(
            dir,
            "0561E1"
        );


    if (
        third.xsens.filename() !=
        "imu_0561E1_2.csv"
    ) {
        fail(
            TEST,
            "third session did not use suffix 2"
        );
    }


    if (failures == failures_before) {
        pass(TEST);
    }
}


/*
 * ---------------------------------------------------------
 * Many virtual devices selecting concurrently
 * ---------------------------------------------------------
 */

void testConcurrentVirtualDevices(
    const std::filesystem::path& root
) {
    constexpr const char* TEST =
        "parallel virtual devices select independent paths";

    const int failures_before =
        failures;

    constexpr std::size_t DEVICE_COUNT =
        16;


    const auto dir =
        root / "parallel";

    std::filesystem::create_directories(
        dir
    );


    /*
     * Give each device a unique MMS+ ID.
     *
     * Also intentionally give different devices different numbers
     * of previous sessions:
     *
     *   device 0 -> suffix 0 free
     *   device 1 -> suffix 1
     *   device 2 -> suffix 2
     *   device 3 -> suffix 3
     *   device 4 -> suffix 0
     *   ...
     */
    std::vector<std::string>
        device_ids(
            DEVICE_COUNT
        );


    std::vector<std::size_t>
        expected_suffix(
            DEVICE_COUNT
        );


    for (
        std::size_t i = 0;
        i < DEVICE_COUNT;
        ++i
    ) {
        device_ids[i] =
            "VIRTUAL_" +
            std::to_string(i);

        expected_suffix[i] =
            i % 4;


        /*
         * Occupy all suffixes before expected_suffix[i].
         *
         * We vary which companion file creates the collision to
         * prove that any session file reserves the suffix.
         */
        for (
            std::size_t suffix = 0;
            suffix < expected_suffix[i];
            ++suffix
        ) {
            const std::string suffix_text =
                suffix == 0
                    ? std::string{}
                    : "_" +
                      std::to_string(
                          suffix
                      );


            switch (suffix % 3) {

            case 0:
                touch(
                    dir /
                    (
                        "imu_" +
                        device_ids[i] +
                        suffix_text +
                        ".csv"
                    )
                );
                break;


            case 1:
                touch(
                    dir /
                    (
                        "battery_" +
                        device_ids[i] +
                        suffix_text +
                        ".csv"
                    )
                );
                break;


            default:
                touch(
                    dir /
                    (
                        "imu_legacy_" +
                        device_ids[i] +
                        suffix_text +
                        ".csv"
                    )
                );
                break;
            }
        }
    }


    std::vector<CsvOutputPaths>
        results(
            DEVICE_COUNT
        );


    /*
     * Make every thread begin chooseUnusedCsvOutputPaths()
     * together.
     */
    std::barrier start_barrier(
        static_cast<std::ptrdiff_t>(
            DEVICE_COUNT
        )
    );


    std::vector<std::thread>
        workers;

    workers.reserve(
        DEVICE_COUNT
    );


    for (
        std::size_t i = 0;
        i < DEVICE_COUNT;
        ++i
    ) {
        workers.emplace_back(
            [
                &dir,
                &device_ids,
                &results,
                &start_barrier,
                i
            ] {
                start_barrier.arrive_and_wait();

                results[i] =
                    chooseUnusedCsvOutputPaths(
                        dir,
                        device_ids[i]
                    );
            }
        );
    }


    for (auto& worker : workers) {
        worker.join();
    }


    /*
     * All selected paths from all devices should be globally unique.
     */
    std::set<std::filesystem::path>
        all_paths;


    for (
        std::size_t i = 0;
        i < DEVICE_COUNT;
        ++i
    ) {
        const std::string suffix =
            expected_suffix[i] == 0
                ? std::string{}
                : "_" +
                  std::to_string(
                      expected_suffix[i]
                  );


        const auto expected_xsens =
            dir /
            (
                "imu_" +
                device_ids[i] +
                suffix +
                ".csv"
            );


        const auto expected_legacy =
            dir /
            (
                "imu_legacy_" +
                device_ids[i] +
                suffix +
                ".csv"
            );


        const auto expected_battery =
            dir /
            (
                "battery_" +
                device_ids[i] +
                suffix +
                ".csv"
            );


        if (
            !pathEquals(
                results[i].xsens,
                expected_xsens
            )
        ) {
            fail(
                TEST,
                "device " +
                std::to_string(i) +
                " selected wrong Xsens path: " +
                results[i].xsens.string()
            );
        }


        if (
            !pathEquals(
                results[i].imu,
                expected_legacy
            )
        ) {
            fail(
                TEST,
                "device " +
                std::to_string(i) +
                " selected wrong legacy path"
            );
        }


        if (
            !pathEquals(
                results[i].battery,
                expected_battery
            )
        ) {
            fail(
                TEST,
                "device " +
                std::to_string(i) +
                " selected wrong battery path"
            );
        }


        /*
         * insert() returns false if the path was already present.
         */
        if (
            !all_paths
                .insert(
                    results[i].xsens
                )
                .second
        ) {
            fail(
                TEST,
                "duplicate Xsens path selected"
            );
        }


        if (
            !all_paths
                .insert(
                    results[i].imu
                )
                .second
        ) {
            fail(
                TEST,
                "duplicate legacy IMU path selected"
            );
        }


        if (
            !all_paths
                .insert(
                    results[i].battery
                )
                .second
        ) {
            fail(
                TEST,
                "duplicate battery path selected"
            );
        }
    }


    const std::size_t expected_path_count =
        DEVICE_COUNT *
        3;


    if (
        all_paths.size() !=
        expected_path_count
    ) {
        fail(
            TEST,
            "expected " +
            std::to_string(
                expected_path_count
            ) +
            " unique paths, got " +
            std::to_string(
                all_paths.size()
            )
        );
    }


    if (failures == failures_before) {
        pass(TEST);
    }
}


int runFilenameCollisionTests()
{
    const std::filesystem::path root =
        makeTempDirectory();


    std::cout
        << "Temporary directory: "
        << root
        << "\n\n";


    try {
        testUnusedDevice(root);

        testThreeExistingCollisions(
            root
        );

        testBatteryCollision(
            root
        );

        testLegacyCollision(
            root
        );

        testDeviceIdSanitization(
            root
        );

        testSequentialSessions(
            root
        );

        testConcurrentVirtualDevices(
            root
        );
    }
    catch (const std::exception& exception) {
        fail(
            "test infrastructure",
            exception.what()
        );
    }


    if (failures == 0) {
        std::error_code error;

        std::filesystem::remove_all(
            root,
            error
        );
    }
    else {
        std::cerr
            << "\nArtifacts preserved at:\n  "
            << root
            << '\n';
    }


    return failures;
}

} // namespace headmotion::app


int main()
{
    std::cout
        << "========================================\n"
        << "HeadMotion Sync Filename Collision Tests\n"
        << "========================================\n\n";


    const int result =
        headmotion::app::
            runFilenameCollisionTests();


    if (result != 0) {
        std::cerr
            << "\n"
            << result
            << " filename collision assertion(s) FAILED\n";

        return 1;
    }


    std::cout
        << "\n"
        << "All filename collision tests PASSED\n";


    return 0;
}