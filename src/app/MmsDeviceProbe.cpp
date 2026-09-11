#include "headmotion/app/MmsDeviceProbe.hpp"

#include "headmotion/transport/SerialConfig.hpp"
#include "headmotion/transport/SerialPortFactory.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace headmotion::app {

    std::string deviceIdFromMmsIdentity(
        const std::string& identity
    ) {
        if (identity.empty()) {
            return {};
        }

        std::size_t end = identity.size();

        while (
            end > 0 &&
            std::isspace(
                static_cast<unsigned char>(
                    identity[end - 1]
                    )
            )
            ) {
            --end;
        }

        if (end == 0) {
            return {};
        }

        const std::size_t separator =
            identity.find_last_of(
                " \t\r\n",
                end - 1
            );

        const std::size_t begin =
            separator == std::string::npos
            ? 0
            : separator + 1;

        if (begin >= end) {
            return {};
        }

        const std::string candidate =
            identity.substr(
                begin,
                end - begin
            );

        /*
         * Current MMS+ identity strings end in a hexadecimal board serial
         * such as 0561F6.  Keep the check deliberately conservative so we
         * do not accidentally treat firmware/model text as a device ID.
         */
        if (
            candidate.size() < 4 ||
            !std::all_of(
                candidate.begin(),
                candidate.end(),
                [](unsigned char ch) {
                    return std::isxdigit(ch) != 0;
                }
            )
            ) {
            return {};
        }

        return candidate;
    }

    std::optional<MmsProbeResult> probeMmsDevice(
        const std::string& port_name
    ) {
        using namespace std::chrono_literals;

        try {
            headmotion::transport::SerialConfig config;

            config.port_name = port_name;
            config.baud_rate = 115200;
            config.data_bits = 8;
            config.stop_bits = 1;
            config.assert_dtr = true;
            config.assert_rts = true;
            config.open_delay = 100ms;

            auto port =
                headmotion::transport::SerialPortFactory::
                create(config);

            port->open();

            const std::vector<std::uint8_t> query = {
                static_cast<std::uint8_t>('?'),
                static_cast<std::uint8_t>('\n')
            };

            port->write(query);

            std::vector<std::uint8_t> response;

            const auto deadline =
                std::chrono::steady_clock::now() +
                1500ms;

            while (
                std::chrono::steady_clock::now() <
                deadline
                ) {
                auto chunk =
                    port->read(
                        256,
                        200ms
                    );

                if (!chunk.empty()) {
                    response.insert(
                        response.end(),
                        chunk.begin(),
                        chunk.end()
                    );
                }

                const std::string text(
                    response.begin(),
                    response.end()
                );

                if (
                    text.find('\n') !=
                    std::string::npos
                    ) {
                    break;
                }
            }

            if (response.empty()) {
                return std::nullopt;
            }

            std::string identity(
                response.begin(),
                response.end()
            );

            while (
                !identity.empty() &&
                (
                    identity.back() == '\n' ||
                    identity.back() == '\r'
                    )
                ) {
                identity.pop_back();
            }

            const bool verified =
                identity.find("MetaMotionS") !=
                std::string::npos ||
                identity.find("MetaWear") !=
                std::string::npos ||
                identity.find("MbientLab") !=
                std::string::npos;

            if (!verified) {
                return std::nullopt;
            }

            return MmsProbeResult{
                .identity = identity
            };

        }
        catch (...) {
            return std::nullopt;
        }
    }

} // namespace headmotion::app
