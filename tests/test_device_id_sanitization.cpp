/*
 * Device-ID filename sanitization unit tests.
 *
 * No MMS+ hardware required.
 *
 * Verifies that hardware/device IDs are safe to embed in CSV filenames.
 */

#include "../src/app/SyncCommand.cpp"

#include <iostream>
#include <string>
#include <vector>

namespace {

struct TestCase {
    std::string input;
    std::string expected;
};

int failures = 0;


void fail(
    const std::string& input,
    const std::string& expected,
    const std::string& actual
) {
    ++failures;

    std::cerr
        << "FAIL\n"
        << "  input:    \"" << input << "\"\n"
        << "  expected: \"" << expected << "\"\n"
        << "  actual:   \"" << actual << "\"\n";
}


void pass(
    const std::string& input,
    const std::string& actual
) {
    std::cout
        << "PASS: \""
        << input
        << "\" -> \""
        << actual
        << "\"\n";
}

} // namespace


namespace headmotion::app {

int runDeviceIdSanitizationTests()
{
    const std::vector<TestCase> tests{
        /*
         * Normal MMS+ hardware ID.
         */
        {
            "0561E1",
            "0561E1"
        },

        /*
         * Spaces become underscores.
         */
        {
            "MMS 123",
            "MMS_123"
        },

        /*
         * Path traversal characters are removed from the edges
         * after being converted to underscores.
         */
        {
            "../../../evil",
            "evil"
        },

        /*
         * ':' and '/' are not valid filename-token characters.
         */
        {
            "abc:def/ghi",
            "abc_def_ghi"
        },

        /*
         * '-' and '_' are explicitly allowed.
         */
        {
            "MMS-123_ABC",
            "MMS-123_ABC"
        },

        /*
         * Leading/trailing invalid characters become underscores,
         * then those edge underscores are stripped.
         */
        {
            "///0561E1///",
            "0561E1"
        },

        {
            "...0561E1...",
            "0561E1"
        },

        /*
         * Interior invalid characters are replaced individually.
         *
         * Runs of underscores are currently NOT collapsed.
         */
        {
            "MMS:::123",
            "MMS___123"
        },

        /*
         * Path-looking input cannot retain directory separators.
         */
        {
            "../../etc/passwd",
            "etc_passwd"
        },

        /*
         * Windows-looking path separators/drive punctuation.
         */
        {
            "C:\\devices\\0561E1",
            "C__devices_0561E1"
        },

        /*
         * Existing underscores at the edges are also stripped.
         */
        {
            "__0561E1__",
            "0561E1"
        },

        /*
         * Everything invalid -> empty token.
         *
         * resolveOutputDeviceId() handles an empty final token with
         * its own fallback when appropriate.
         */
        {
            "///:::...",
            ""
        },

        /*
         * Empty stays empty.
         */
        {
            "",
            ""
        }
    };


    for (const auto& test : tests) {
        const std::string actual =
            sanitizeFilenameToken(
                test.input
            );

        if (actual != test.expected) {
            fail(
                test.input,
                test.expected,
                actual
            );
        }
        else {
            pass(
                test.input,
                actual
            );
        }
    }


    /*
     * Extra safety property:
     *
     * No sanitized non-empty token should contain a slash,
     * backslash, colon, or dot from our test inputs.
     */
    for (const auto& test : tests) {
        const std::string sanitized =
            sanitizeFilenameToken(
                test.input
            );

        if (
            sanitized.find('/') !=
                std::string::npos ||
            sanitized.find('\\') !=
                std::string::npos ||
            sanitized.find(':') !=
                std::string::npos ||
            sanitized.find('.') !=
                std::string::npos
        ) {
            ++failures;

            std::cerr
                << "FAIL: unsafe character survived sanitization\n"
                << "  input:  \""
                << test.input
                << "\"\n"
                << "  output: \""
                << sanitized
                << "\"\n";
        }
    }


    if (failures == 0) {
        std::cout
            << "\nPASS: no unsafe filename characters survived\n";
    }


    return failures;
}

} // namespace headmotion::app


int main()
{
    std::cout
        << "========================================\n"
        << "HeadMotion Device-ID Sanitization Tests\n"
        << "========================================\n\n";


    const int result =
        headmotion::app::
            runDeviceIdSanitizationTests();


    if (result != 0) {
        std::cerr
            << "\n"
            << result
            << " sanitization assertion(s) FAILED\n";

        return 1;
    }


    std::cout
        << "\n"
        << "All device-ID sanitization tests PASSED\n";


    return 0;
}