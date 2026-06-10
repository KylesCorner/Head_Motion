#include "headmotion/transport/SerialPortFactory.hpp"

#include <iostream>

namespace headmotion::app {

int runScanPortsCommand() {
    const auto ports = headmotion::transport::SerialPortFactory::listPorts();

    if (ports.empty()) {
        std::cout << "No serial ports found.\n";
        return 0;
    }

    std::cout << "Serial ports:\n";

    for (const auto& port : ports) {
        std::cout << "  " << port.path;

        if (!port.symlink_path.empty()) {
            std::cout << "  via " << port.symlink_path;
        }

        if (!port.display_name.empty() && port.display_name != port.path) {
            std::cout << "  [" << port.display_name << "]";
        }

        if (port.likely_mms) {
            std::cout << "  likely MMS";
        }

        std::cout << "\n";
    }

    return 0;
}

} // namespace headmotion::app
